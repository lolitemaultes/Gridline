// Gridline: the cost of playing a long resampled sample (K-159, GitHub issue:
// a few notes held in the Sampler put the audio load at 100%).
//
// A 17.5 s, 44.1 kHz stereo file played in a 48 kHz project: every frame of
// every note goes through the band-limited reader (engine/pitch/Kernel.h),
// about 54 taps per read at the realtime kernel and more when pitched up. The
// reader of contiguous audio now uses precomputed polyphase rows at the fixed
// cutoff and a stepped table walk above it.
//
// Checks:
//  - the fast reads equal the reference read (readWith) to rounding, for the
//    three kernels, slow and fast rates, near the ends of the audio too;
//  - the fast reads are at least twice as fast as the reference (machine
//    independent: both timed in this process);
//  - an 8-note chord on the Sampler renders far faster than real time, through
//    the generator and through the live engine (its own load meter);
//  - the other sample players with the same file stay within real time.
// Prints the cost per voice of each. GL_SAMPLER_CPU_DUMP=<path> writes the
// chord's render (raw float, L then R) for before/after comparisons.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/pitch/Kernel.h"
#include "../Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000;
constexpr int kBlock = 512;
// Timing checks only in optimised builds without sanitizers (they slow the reads down unevenly).
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) || __has_feature(thread_sanitizer)
#define GL_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GL_TEST_SANITIZED 1
#endif
#if defined(GL_TEST_SANITIZED) || !defined(NDEBUG)
constexpr bool kTimed = false;
#else
constexpr bool kTimed = true;
#endif

std::vector<float> testSignal(size_t n, int channel) {
  std::vector<float> x(n);
  uint32_t seed = 12345u + static_cast<uint32_t>(channel);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const double nz = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.2;
    const double t = static_cast<double>(i) / 44100.0;
    x[i] = static_cast<float>(0.3 * std::sin(2 * kPi * (110.0 + 3 * channel) * t) + 0.2 * std::sin(2 * kPi * 2750.0 * t) + nz);
  }
  return x;
}

std::string writeLong(const std::string& name, double sec) {
  const auto n = static_cast<size_t>(sec * 44100);
  const std::vector<float> l = testSignal(n, 0), r = testSignal(n, 1);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = l[i], b.at(1, i) = r[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, 44100, 24);
  return path;
}

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---- the reader: fast paths against the reference
void kernelChecks() {
  const size_t n = 200000;
  const std::vector<float> l = testSignal(n, 0), r = testSignal(n, 1);
  const float* xs[2] = {l.data(), r.data()};
  const auto nn = static_cast<int64_t>(n);
  const char* names[3] = {"realtime", "offline", "compact"};
  double worst = 0;
  bool finite = true;
  for (int q = 0; q < 3; ++q) {
    const pitch::SincTable& t = pitch::table(static_cast<pitch::Quality>(q));
    for (double rate : {0.125, 0.5, 0.9187, 1.0, 1.03, 1.0444, 1.2, 1.9, 3.7, 6.5, 13.0, 40.0}) {
      double md = 0;
      uint32_t seed = 99;
      for (int k = 0; k < 3000; ++k) {
        seed = seed * 1664525u + 1013904223u;
        // positions all over, and right at both ends (the bounded reads)
        double pos = static_cast<double>(seed >> 8) / 16777216.0 * static_cast<double>(n - 1);
        if (k % 10 == 1) pos = std::fmod(pos, 120.0);
        if (k % 10 == 2) pos = static_cast<double>(n - 1) - std::fmod(pos, 120.0);
        if (k % 10 == 3) pos = std::floor(pos);  // on a frame
        float fast[2], ref[2];
        pitch::readArrays<2>(xs, nn, pos, rate, t, fast);
        pitch::readWith<2>([&](int64_t i, int c) { return i >= 0 && i < nn ? xs[c][i] : 0.0f; }, pos, rate, t, ref);
        float one = pitch::read(l.data(), nn, pos, rate, t);
        finite = finite && std::isfinite(fast[0]) && std::isfinite(fast[1]) && std::isfinite(one);
        md = std::max({md, static_cast<double>(std::fabs(fast[0] - ref[0])), static_cast<double>(std::fabs(fast[1] - ref[1])),
                       static_cast<double>(std::fabs(one - ref[0]))});
      }
      worst = std::max(worst, md);
      if (md > 3e-6) std::printf("# %s rate %.4f: fast read differs by %.3g\n", names[q], rate, md);
    }
  }
  // A read depends only on the frames within its reach: the same read of arrays
  // that end just past its reach (zeros beyond) gives the same bits as of longer
  // ones (a stream read while it fills, then replayed, stays bit for bit).
  bool sameBits = true;
  for (int q = 0; q < 3; ++q) {
    const pitch::SincTable& t = pitch::table(static_cast<pitch::Quality>(q));
    for (double rate : {0.5, 0.9187, 1.2, 3.7, 13.0}) {
      const int reach = pitch::reach(rate, t);
      for (double pos : {5000.0, 5000.37, 7777.731}) {
        const auto end = static_cast<int64_t>(std::floor(pos)) + reach + 1;  // frames [0, end): everything the read weighs
        std::vector<float> l2(l.begin(), l.begin() + end), r2(r.begin(), r.begin() + end);
        const float* xs2[2] = {l2.data(), r2.data()};
        float longer[2], shorter[2];
        pitch::readArrays<2>(xs, nn, pos, rate, t, longer);
        pitch::readArrays<2>(xs2, end, pos, rate, t, shorter);
        sameBits = sameBits && longer[0] == shorter[0] && longer[1] == shorter[1];
      }
    }
  }
  expect(sameBits, "reader: a read gives the same bits whatever the arrays hold beyond its reach");
  std::printf("# reader: largest difference from the reference %.3g (%.1f dBFS)\n", worst, 20 * std::log10(worst + 1e-30));
  expect(finite, "reader: fast reads are finite");
  expect(worst < 3e-6, "reader: fast reads equal the reference read to rounding (under -110 dBFS)");

  // Speed: the realtime kernel at the rates a resampled note reads at.
  const pitch::SincTable& t = pitch::table(pitch::Quality::Realtime);
  auto timeReads = [&](bool fast, double rate) {
    const int reads = 60000;
    double pos = 1000.37, acc = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < reads; ++k) {
      float o[2];
      if (fast)
        pitch::readArrays<2>(xs, nn, pos, rate, t, o);
      else
        pitch::readWith<2>([&](int64_t i, int c) { return xs[c][i]; }, pos, rate, t, o);
      acc += static_cast<double>(o[0]) + o[1];
      pos += rate;
      if (pos > static_cast<double>(n) - 2000) pos = 1000.37;
    }
    const double s = seconds(t0);
    return acc == 12345.678 ? 0.0 : s;  // (acc keeps the reads)
  };
  for (double rate : {0.9187, 1.5}) {
    double best = 0;
    for (int tries = 0; tries < 3 && best < 2.0; ++tries) {  // a busy machine gets three tries
      const double ref = timeReads(false, rate), fast = timeReads(true, rate);
      best = std::max(best, ref / std::max(fast, 1e-9));
    }
    std::printf("# reader: rate %.4f fast read %.1fx the reference speed\n", rate, best);
    expect(!kTimed || best >= (rate < 1.04 ? 2.0 : 1.3), (std::string("reader: fast read at least ") + std::string(rate < 1.04 ? "2x" : "1.3x") + " the reference speed (rate " + std::to_string(rate) + ")").c_str());
  }
}

// ---- instruments
struct Run {
  double seconds = 0, audio = 0;
  std::vector<float> L, R;
};

// `notes` started together; renders `sec` seconds in 512-frame blocks at 48 kHz.
Run render(const std::string& type, const PluginState& s, const std::vector<int>& notes, double sec, bool keep) {
  ChannelAux aux;
  prepareChannelAux(s, aux);
  auto gen = createGenerator(type);
  gen->prepareHost(s, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  const auto frames = static_cast<size_t>(sec * kSr);
  Run r;
  std::vector<float> L(kBlock), R(kBlock);
  if (keep) r.L.reserve(frames), r.R.reserve(frames);
  uint32_t id = 1;
  for (int nn : notes) gen->noteOn(id++, nn, 0.9f, 0);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < frames; i += kBlock) {
    const int n = static_cast<int>(std::min<size_t>(kBlock, frames - i));
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    gen->process(s, aux, ctx, L.data(), R.data(), n);
    if (keep) r.L.insert(r.L.end(), L.begin(), L.begin() + n), r.R.insert(r.R.end(), R.begin(), R.begin() + n);
  }
  r.seconds = seconds(t0);
  r.audio = sec;
  return r;
}

// The live engine: the channel in a 48 kHz project, the notes played by hand, 512-frame callbacks.
struct EngineRun {
  double seconds = 0, audio = 0;
  float meter = 0;
};
EngineRun engineRun(const PluginState& plugin, const std::vector<int>& notes, double sec) {
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "smp";
  ch.plugin = plugin;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  ProgramBuilder builder;
  builder.setSampleRate(kSr);
  auto prog = builder.build(s);
  AudioEngine eng(kSr);
  eng.setProgram(prog);
  std::vector<float> L(kBlock), R(kBlock);
  for (int i = 0; i < 20; ++i) eng.process(L.data(), R.data(), kBlock);
  for (int nn : notes) eng.noteOn(ch.id, nn, 0.9f);
  EngineRun r;
  const auto frames = static_cast<size_t>(sec * kSr);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < frames; i += kBlock) eng.process(L.data(), R.data(), kBlock);
  r.seconds = seconds(t0);
  r.audio = static_cast<double>(frames) / kSr;
  r.meter = eng.cpuLoad();
  return r;
}

double rmsOf(const std::vector<float>& x) {
  double s = 0;
  for (float v : x) s += static_cast<double>(v) * v;
  return x.empty() ? 0.0 : std::sqrt(s / static_cast<double>(x.size()));
}

}  // namespace

int main() {
  kernelChecks();

  const std::string path = writeLong("gl_sampler_cpu.wav", 17.47);
  sampleCache().clear();
  const std::vector<int> chord8 = {48, 52, 55, 60, 64, 67, 72, 76};  // two octaves around the root (C4)
  const std::vector<int> one = {60};
  PluginState smp = defaultPluginState("gl.sampler");
  smp.data = path;

  render("gl.sampler", smp, chord8, 0.25, false);  // decode and warm up
  const Run a = render("gl.sampler", smp, one, 3.0, false);
  Run b;
  for (int tries = 0; tries < 3; ++tries) {  // the best of three (a busy machine)
    Run k = render("gl.sampler", smp, chord8, 4.0, true);
    if (tries == 0 || k.seconds < b.seconds) b = std::move(k);
  }
  const double factor = b.audio / std::max(b.seconds, 1e-9);
  std::printf("# sampler: 1 note %.2f ms per audio second, 8 notes %.2f ms (%.0fx real time)\n", 1000 * a.seconds / a.audio,
              1000 * b.seconds / b.audio, factor);
  expect(rmsOf(b.L) > 0.05 && rmsOf(b.R) > 0.05, "sampler: the chord sounds");
  bool finite = true;
  for (size_t i = 0; i < b.L.size(); ++i) finite = finite && std::isfinite(b.L[i]) && std::isfinite(b.R[i]);
  expect(finite, "sampler: the chord's render is finite");
  // Measured (Release, busy machines): 26x on a desktop i7 (macOS, clang), 22x on a 2022 laptop i5 (Linux, gcc), 14x
  // there before the fast reader. 8x leaves room for slow CI runners.
  // Shared CI runners (CI set) are noisy: 3x there still catches the old overload.
  expect(!kTimed || factor >= (std::getenv("CI") ? 3.0 : 8.0), "sampler: an 8-note chord of a long 44.1 kHz sample at 48 kHz renders at least 8x faster than real time");
  if (const char* dump = std::getenv("GL_SAMPLER_CPU_DUMP")) {
    if (FILE* f = std::fopen(dump, "wb")) {
      std::fwrite(b.L.data(), sizeof(float), b.L.size(), f);
      std::fwrite(b.R.data(), sizeof(float), b.R.size(), f);
      std::fclose(f);
    }
  }

  // The live engine with its own load meter: 3 and 8 notes held, and all 32 voices
  // sounding (one-shot notes play the whole file: chords played again and again pile up).
  std::vector<int> many;
  for (int k = 0; k < 32; ++k) many.push_back(k < 8 ? chord8[static_cast<size_t>(k)] : 40 + k);
  for (size_t k : {size_t(3), size_t(8), size_t(32)}) {
    EngineRun best;
    for (int tries = 0; tries < 3; ++tries) {
      const EngineRun e = engineRun(smp, std::vector<int>(many.begin(), many.begin() + static_cast<std::ptrdiff_t>(k)), 3.0);
      if (tries == 0 || e.seconds < best.seconds) best = e;
    }
    std::printf("# engine: %zu notes %.2f ms per audio second, load meter %.1f%%\n", k, 1000 * best.seconds / best.audio, 100 * best.meter);
    // The meter is wall-clock load: shared CI runners (CI set) get a wider limit that still
    // catches the old overload (close to 100%); a local run keeps the strict 12.5%.
    const bool onCi = std::getenv("CI") != nullptr;
    const float limit = onCi ? 0.5f : 0.125f;
    if (k == 8) expect(!kTimed || best.meter < limit, onCi ? "engine: 8 notes held on the Sampler read under 50% on the load meter (CI runner)" : "engine: 8 notes held on the Sampler read under 12.5% on the load meter");
  }

  // The other sample players with the same long file (cost per voice, printed; all within real time).
  for (const char* t : {"gl.slicer", "gl.pads", "gl.granular"}) {
    PluginState s = defaultPluginState(t);
    s.data = path;
    const Run o = render(t, s, one, 1.5, false);
    const Run c = render(t, s, chord8, 1.5, false);
    std::printf("# %s: 1 note %.2f ms per audio second, 8 notes %.2f ms\n", t, 1000 * o.seconds / o.audio, 1000 * c.seconds / c.audio);
    expect(c.seconds < c.audio, (std::string(t) + ": an 8-note chord renders faster than real time").c_str());
  }
  fs::remove(path);
  return finish();
}

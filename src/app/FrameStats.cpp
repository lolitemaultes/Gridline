// Gridline — live frame statistics (see FrameStats.h).
#include "FrameStats.h"
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "../host/ui/NativeView.h"

namespace gl::fstats {

namespace {
std::atomic<bool> gOn{false};
std::atomic<int> gAudioBlocks{0}, gAudioLate{0};
std::atomic<int> gAudioPeak{0};  // busiest block, per mille of its period
double gPart[kParts] = {};
int gFrames = 0;
double gMaxFrame = 0;
uint64_t gLastFrame = 0, gWindowStart = 0;
double gCpu0 = 0;

uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
double cpuSeconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}
}  // namespace

void enable(bool on) { gOn = on; }

void audioBlock(double busy, double period) {
  if (!gOn.load(std::memory_order_relaxed) || period <= 0) return;
  gAudioBlocks.fetch_add(1, std::memory_order_relaxed);
  if (busy > period) gAudioLate.fetch_add(1, std::memory_order_relaxed);
  const int pm = static_cast<int>(busy / period * 1000.0);
  int prev = gAudioPeak.load(std::memory_order_relaxed);
  while (pm > prev && !gAudioPeak.compare_exchange_weak(prev, pm, std::memory_order_relaxed)) {
  }
}
bool enabled() { return gOn.load(std::memory_order_relaxed); }

const char* partName(Part p) {
  switch (p) {
    case Events: return "events";
    case Tick: return "tick";
    case Ui: return "ui";
    case Present: return "present";
    case PluginWindows: return "pluginwin";
    case Wait: return "wait";
    default: return "?";
  }
}

void add(Part p, double ms) {
  if (gOn) gPart[p] += ms;
}

uint64_t stamp() { return gOn ? nowNs() : 0; }
void since(Part p, uint64_t t0) {
  if (gOn && t0) add(p, static_cast<double>(nowNs() - t0) * 1e-6);
}

Timer::Timer(Part p) : p_(p), t0_(gOn ? nowNs() : 0) {}
Timer::~Timer() {
  if (gOn) add(p_, static_cast<double>(nowNs() - t0_) * 1e-6);
}

void endFrame() {
  if (!gOn) return;
  const uint64_t t = nowNs();
  if (gWindowStart == 0) {
    gWindowStart = gLastFrame = t;
    gCpu0 = cpuSeconds();
    std::fill(gPart, gPart + kParts, 0.0);
    host::native::takeDrawStats();
    return;
  }
  gMaxFrame = std::max(gMaxFrame, static_cast<double>(t - gLastFrame) * 1e-6);
  gLastFrame = t;
  ++gFrames;
  const double span = static_cast<double>(t - gWindowStart) * 1e-9;
  if (span < 1.0) return;
  const double cpu = cpuSeconds();
  const host::native::DrawStats d = host::native::takeDrawStats();
  std::fprintf(stderr, "[frame-stats] fps %.1f | frame avg %.1f max %.1f ms |", gFrames / span, span * 1000.0 / gFrames, gMaxFrame);
  for (int p = 0; p < kParts; ++p) std::fprintf(stderr, " %s %.2f", partName(static_cast<Part>(p)), gPart[p] / gFrames);
  std::fprintf(stderr, " | cpu %.0f%% | plugin ui/s: views %.0f layers %.0f gl %.0f metal %.0f | runloop %.0f/s", (cpu - gCpu0) / span * 100.0,
               static_cast<double>(d.views) / span, static_cast<double>(d.layers) / span, static_cast<double>(d.gl) / span, static_cast<double>(d.metal) / span,
               static_cast<double>(d.runLoopTurns) / span);
  std::fprintf(stderr, " | audio blocks %d late %d peak %.0f%%\n", gAudioBlocks.exchange(0), gAudioLate.exchange(0), gAudioPeak.exchange(0) / 10.0);
  gCpu0 = cpu;
  gWindowStart = t;
  gFrames = 0;
  gMaxFrame = 0;
  std::fill(gPart, gPart + kParts, 0.0);
}

}  // namespace gl::fstats

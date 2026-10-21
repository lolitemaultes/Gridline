#include "FrameProfile.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <new>
#include <vector>

// ---- allocation counting (app binary only; sanitizers keep their own allocator) ----
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GL_PROF_COUNT_ALLOCS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define GL_PROF_COUNT_ALLOCS 0
#endif
#endif
#ifndef GL_PROF_COUNT_ALLOCS
#define GL_PROF_COUNT_ALLOCS 1
#endif

namespace {
thread_local uint64_t tAllocs = 0;
}

#if GL_PROF_COUNT_ALLOCS
void* operator new(std::size_t n) {
  ++tAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  ++tAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  ++tAllocs;
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  ++tAllocs;
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

namespace gl::prof {

namespace {

struct Sample {
  double cpuMs = 0, wallMs = 0;
  uint64_t allocs = 0;
};

struct State {
  bool on = false;
  int warmup = 0, seen = 0;
  std::vector<std::string> order;            // window names, first-seen order
  std::vector<const char*> keys;             // name pointers (fast path) parallel to order
  std::vector<Sample> cur;                   // this frame, indexed like order
  std::vector<std::vector<Sample>> frames;   // recorded frames (may be shorter than order)
  int64_t frameCpu0 = 0, frameWall0 = 0;
  uint64_t frameAlloc0 = 0;
};
State& st() {
  static State s;
  return s;
}

int64_t threadCpuNs() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  auto ns = [](const FILETIME& t) { return static_cast<int64_t>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100; };
  return ns(k) + ns(u);
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}
int64_t wallNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const size_t i = static_cast<size_t>(pos);
  const double f = pos - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

void add(State& s, const char* name, int64_t cpuNs, int64_t wallNs, uint64_t allocs) {
  size_t k = 0;
  while (k < s.keys.size() && s.keys[k] != name && s.order[k] != name) ++k;
  if (k == s.keys.size()) {  // first time this window is seen (allocates once)
    s.order.emplace_back(name);
    s.keys.push_back(name);
    s.cur.resize(s.keys.size());
  }
  Sample& x = s.cur[k];
  x.cpuMs += static_cast<double>(cpuNs) * 1e-6;
  x.wallMs += static_cast<double>(wallNs) * 1e-6;
  x.allocs += allocs;
}

}  // namespace

void enable(bool on) { st().on = on; }
bool enabled() { return st().on; }
uint64_t threadAllocs() { return tAllocs; }
void setWarmup(int frames) { st().warmup = std::max(0, frames); }
int recordedFrames() { return static_cast<int>(st().frames.size()); }

Scope::Scope(const char* name) : name_(name) {
  if (!st().on) return;
  cpu0_ = threadCpuNs();
  wall0_ = wallNs();
  alloc0_ = tAllocs;
}

Scope::~Scope() {
  State& s = st();
  if (!s.on) return;
  const int64_t cpu1 = threadCpuNs(), wall1 = wallNs();
  const uint64_t alloc1 = tAllocs;
  add(s, name_, cpu1 - cpu0_, wall1 - wall0_, alloc1 - alloc0_);
}

void beginFrame() {
  State& s = st();
  if (!s.on) return;
  s.frameCpu0 = threadCpuNs();
  s.frameWall0 = wallNs();
  s.frameAlloc0 = tAllocs;
}

void endFrame() {
  State& s = st();
  if (!s.on) return;
  add(s, "frame", threadCpuNs() - s.frameCpu0, wallNs() - s.frameWall0, tAllocs - s.frameAlloc0);
  if (s.seen++ >= s.warmup) s.frames.push_back(s.cur);
  std::fill(s.cur.begin(), s.cur.end(), Sample{});
}

bool writeJson(const std::string& path, const std::string& label) {
  State& s = st();
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f, "{\n  \"label\": \"");
  for (char c : label) {
    if (c == '"' || c == '\\') std::fputc('\\', f);
    std::fputc(static_cast<unsigned char>(c) < 0x20 ? ' ' : c, f);
  }
  std::fprintf(f, "\",\n  \"frames\": %zu,\n  \"windows\": {", s.frames.size());
  for (size_t w = 0; w < s.order.size(); ++w) {
    const std::string& name = s.order[w];
    std::vector<double> cpu, wall, alloc;
    for (const auto& fr : s.frames) {
      const Sample x = w < fr.size() ? fr[w] : Sample{};
      cpu.push_back(x.cpuMs);
      wall.push_back(x.wallMs);
      alloc.push_back(static_cast<double>(x.allocs));
    }
    double mean = 0;
    for (double v : cpu) mean += v;
    mean = cpu.empty() ? 0 : mean / static_cast<double>(cpu.size());
    std::fprintf(f,
                 "%s\n    \"%s\": {\"cpu_ms_median\": %.4f, \"cpu_ms_mean\": %.4f, \"cpu_ms_p95\": %.4f, \"cpu_ms_max\": %.4f, "
                 "\"wall_ms_median\": %.4f, \"allocs_median\": %.1f}",
                 w ? "," : "", name.c_str(), percentile(cpu, 0.5), mean, percentile(cpu, 0.95),
                 cpu.empty() ? 0.0 : *std::max_element(cpu.begin(), cpu.end()), percentile(wall, 0.5), percentile(alloc, 0.5));
  }
  std::fprintf(f, "\n  },\n  \"perFrame\": [");
  for (size_t i = 0; i < s.frames.size(); ++i) {
    std::fprintf(f, "%s\n    {", i ? "," : "");
    bool first = true;
    for (size_t w = 0; w < s.frames[i].size(); ++w) {
      const Sample& x = s.frames[i][w];
      std::fprintf(f, "%s\"%s\": [%.4f, %llu]", first ? "" : ", ", s.order[w].c_str(), x.cpuMs,
                   static_cast<unsigned long long>(x.allocs));
      first = false;
    }
    std::fprintf(f, "}");
  }
  std::fprintf(f, "\n  ]\n}\n");
  return std::fclose(f) == 0;
}

}  // namespace gl::prof

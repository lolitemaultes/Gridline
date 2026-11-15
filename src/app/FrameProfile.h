// Gridline — per-window UI frame profiler (dev flag `--profile-frames <json>`, J-25).
// Each drawn window is wrapped in a Scope that adds the UI thread's CPU time
// (thread clock: unaffected by other processes on a loaded machine), wall time
// and heap allocation count to the current frame. endFrame() closes a frame;
// writeJson() stores per-window medians / p95 / max plus every frame.
#pragma once
#include <cstdint>
#include <string>

namespace gl::prof {

// Off by default: Scope then costs one branch.
void enable(bool on);
bool enabled();
// K-112 (dev, --switch-check): while on, every Scope that closes writes its wall time to stderr
// ("[frame-trace] frame <n> <name> <ms>"), a timing log of one frame's parts.
void setTrace(bool on, int frame = -1);
// CPU time the calling thread has used so far (ns): unlike wall time, it does not grow when other
// processes take the machine (the switch check measures the switch frame with it).
long long threadCpuNow();

// Heap allocations made by the calling thread so far (counted only in builds
// without sanitizers; 0 otherwise).
uint64_t threadAllocs();

class Scope {
 public:
  explicit Scope(const char* name);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const char* name_;
  int64_t cpu0_ = 0, wall0_ = 0;
  uint64_t alloc0_ = 0;
};

// Opens / closes a frame; the whole frame is recorded as "frame" (frames
// before `warmup` are not recorded).
void beginFrame();
void endFrame();
void setWarmup(int frames);
int recordedFrames();
// Writes {"frames":N, "windows":{name:{cpu_ms_median,…}}, "perFrame":[…]}.
bool writeJson(const std::string& path, const std::string& label);

}  // namespace gl::prof

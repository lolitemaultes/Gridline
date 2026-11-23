// Gridline - K-142 (0.7.8): names for the threads Gridline makes, and what the CPU report
// (`gridline-app --cpu-report`, app/CpuReport.h) reads about them.
//
// Every thread Gridline starts names itself first thing (setName), so the system's tools, crash
// reports and the CPU report tell them apart: "gl-audio-pool", "gl-index", "gl-autosave", ...
// (15 characters at most: Linux keeps no more).
//
// Long-lived workers call woke() each time they wake from a wait to look for work. It counts only
// while a CPU report records (one relaxed load and a branch otherwise), so an idle Gridline can be
// checked for workers that keep waking with nothing to do.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::threads {

// Names the calling thread (cut to 15 characters). Never fails loudly.
void setName(const char* name) noexcept;
// The calling thread's name as the system has it ("" when unnamed or unknown).
std::string currentName();

// Wakeup counting (CPU report).
extern std::atomic<bool> gCountWakes;
void wokeSlow() noexcept;
inline void woke() noexcept {
  if (gCountWakes.load(std::memory_order_relaxed)) wokeSlow();
}
void setCountWakes(bool on);
struct Wakes {
  std::string name;
  uint64_t count = 0;
};
// Wakeups counted per worker name since counting began (threads of one name summed).
std::vector<Wakes> wakes();

// One thread of this process, as the system reports it now.
struct ThreadTime {
  uint64_t id = 0;     // the system's thread id (stable for the thread's life)
  std::string name;    // its name ("" when unnamed)
  double cpu = 0;      // user + system processor time so far, seconds
};
// Every thread of this process (empty where the system does not say: Windows for now).
std::vector<ThreadTime> sampleThreads();

}  // namespace gl::threads

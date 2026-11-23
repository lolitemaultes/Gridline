// Gridline - K-142 (0.7.8): the idle CPU report, a development flag for hidden runs.
//
//   gridline-app <project> --hidden --cpu-report <seconds> [--cpu-report-out <file.json>]
//                [--cpu-settle <seconds>] [--cpu-states visible,unfocused,minimised,hidden]
//
// Once the project has loaded, for each state in turn: the window is put in that state (a hidden
// run cannot really minimise or hide its window: the events the window system would send are
// given instead), the run waits `settle` seconds (meters fall, frames slow to the idle pace) and
// then records for `seconds`:
//  * this process's processor time (user + system), context switches and (macOS) system calls;
//  * each thread's processor time by name (every thread Gridline makes names itself: ThreadName.h);
//  * frames drawn, audio device callbacks and engine blocks;
//  * the blocks each instrument and effect ran its DSP (AudioEngine::setCallCounting);
//  * file system questions asked through core/SafeFs.h;
//  * how often each worker thread woke (threads::woke).
// "cpu_split" (K-142z): "own" is what Gridline's threads spend (every thread named gl-*, the
// main thread included whole, its event waits and the window system's calls on it too), except
// the audio device thread: the driver makes it, so only the time inside Gridline's callback is
// own and the rest (the driver waiting and waking) is the system's. "system" is everything else:
// the audio driver's share, CoreAudio and HAL threads, the display link, SDL's own threads, the
// system's unnamed worker threads. "total" is the whole process (own + system, near enough).
//
// The JSON goes to the file (default <config>/cpu-report.json); then the run quits, its changes
// discarded. Interactive pacing (IdlePace) stays on, as in a real session.
//
// Off (no --cpu-report), nothing here runs: the counters it reads are each one relaxed load and a
// branch at their call sites, and nothing heard or drawn changes either way.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace gl {
class App;
}

namespace gl::cpureport {

struct Options {
  double seconds = 0;   // recorded per state (0: off)
  double settle = 10;   // waited after the load and after each state change
  std::string out;      // JSON file ("": <config>/cpu-report.json)
  std::vector<std::string> states{"visible"};
};

void configure(const Options& o);
bool on();
// "visible,hidden" -> {"visible", "hidden"}; false when a state is not one of the four.
bool parseStates(const std::string& list, std::vector<std::string>& out);

// Audio device thread (lock-free): one device callback, counted, and the processor time the
// callback itself takes on its thread (K-142z). The device thread belongs to the audio driver
// (SDL's dummy driver, CoreAudio, ASIO): what it spends outside the callback, waiting and waking,
// is the driver's, so the report splits that thread into Gridline's work (the callback) and the
// driver's share. Off: one relaxed load and a branch at the start and at the end of a callback.
extern std::atomic<bool> gCounting;
extern std::atomic<uint64_t> gCallbacks;
extern std::atomic<uint64_t> gCallbackCpuNs;
long long callingThreadCpuNs() noexcept;  // the calling thread's processor time so far (0 where unknown)
struct AudioCallbackScope {
  long long cpu0 = -1;
  AudioCallbackScope() noexcept {
    if (gCounting.load(std::memory_order_relaxed)) {
      gCallbacks.fetch_add(1, std::memory_order_relaxed);
      cpu0 = callingThreadCpuNs();
    }
  }
  ~AudioCallbackScope() {
    if (cpu0 < 0) return;
    const long long d = callingThreadCpuNs() - cpu0;
    if (d > 0) gCallbackCpuNs.fetch_add(static_cast<uint64_t>(d), std::memory_order_relaxed);
  }
  AudioCallbackScope(const AudioCallbackScope&) = delete;
  AudioCallbackScope& operator=(const AudioCallbackScope&) = delete;
};

// The main loop, once per turn: runs the report (`drew`: a frame was drawn this turn, taking
// `frameCpuNs` of the UI thread's processor time, `glCpuNs` of it inside the OpenGL calls that draw
// and present it). True once it is written (the run then quits, discarding changes).
bool frame(App& app, SDL_Window* window, bool drew, long long frameCpuNs, long long glCpuNs = 0);
// The main window's OpenGL renderer (GL_RENDERER), recorded in the report with whether it is a
// software one (Mesa's llvmpipe, softpipe or swrast, SwiftShader, Apple's software renderer): there
// drawing runs on the processor, in the interface thread and the renderer's own threads.
void setRenderer(const char* renderer);
bool softwareRenderer(const char* renderer);
// K-142: seconds until the report next has something to do (its next state or the end of a
// recording); an idle loop that draws nothing wakes for it.
double secondsToNext();

}  // namespace gl::cpureport

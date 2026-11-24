// Gridline - K-142 (0.7.8): the idle CPU report (see CpuReport.h).
#include "CpuReport.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>
#include <time.h>
#include "../core/Json.h"
#include "../core/LayoutPreset.h"
#include "../core/Platform.h"
#include "../core/SafeFs.h"
#include "../core/ThreadName.h"
#include "../engine/AudioEngine.h"
#include "../engine/Program.h"
#include "App.h"
#include "GridlineVersion.h"
#include "LayoutPresets.h"
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/utsname.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace gl::cpureport {

std::atomic<bool> gCounting{false};
std::atomic<uint64_t> gCallbacks{0};
std::atomic<uint64_t> gCallbackCpuNs{0};

long long callingThreadCpuNs() noexcept {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  auto ns = [](const FILETIME& t) { return static_cast<long long>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100; };
  return ns(k) + ns(u);
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

MainLoop& mainLoop() {
  static MainLoop m;
  return m;
}

namespace {

Options gOpt;

double nowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Process-wide counters the system keeps.
struct ProcessSnap {
  double cpu = 0, user = 0, sys = 0;
  uint64_t voluntarySwitches = 0, involuntarySwitches = 0;
  uint64_t syscallsUnix = 0, syscallsMach = 0, machSwitches = 0;  // macOS
  uint64_t diskRead = 0, diskWritten = 0;                         // macOS
};

ProcessSnap processSnap() {
  ProcessSnap s;
  s.cpu = platform::cpuSeconds();
#if !defined(_WIN32)
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  s.user = static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) * 1e-6;
  s.sys = static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) * 1e-6;
  s.voluntarySwitches = static_cast<uint64_t>(ru.ru_nvcsw);
  s.involuntarySwitches = static_cast<uint64_t>(ru.ru_nivcsw);
#endif
#if defined(__APPLE__)
  task_events_info_data_t ev{};
  mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&ev), &n) == KERN_SUCCESS) {
    s.syscallsUnix = static_cast<uint64_t>(ev.syscalls_unix);
    s.syscallsMach = static_cast<uint64_t>(ev.syscalls_mach);
    s.machSwitches = static_cast<uint64_t>(ev.csw);
  }
  rusage_info_v2 ri{};
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
    s.diskRead = ri.ri_diskio_bytesread;
    s.diskWritten = ri.ri_diskio_byteswritten;
  }
#endif
  return s;
}

struct NodeKey {
  int kind = 0;  // 0 instrument (channel), 1 effect (mixer slot)
  ObjectId owner = kNoId;
  int slot = -1;
  bool operator<(const NodeKey& o) const {
    return kind != o.kind ? kind < o.kind : owner != o.owner ? owner < o.owner : slot < o.slot;
  }
};
struct NodeStart {
  uint64_t rest = 0, run = 0;
};

struct Run {
  std::string state;
  bool recording = false;
  double enter = 0, start = 0;
  int frames = 0;
  double frameCpuNs = 0;  // the UI thread's processor time inside the frames drawn
  double glCpuNs = 0;     // of that, inside the OpenGL calls that draw and present them
  ProcessSnap p0;
  std::vector<threads::ThreadTime> t0;
  uint64_t graphBlocks0 = 0, poolWakes0 = 0, counted0 = 0, audible0 = 0, slept0 = 0, longRest0 = 0;
  std::map<NodeKey, NodeStart> nodes0;
  MainLoop main0;
};

enum class Phase { WaitLoad, Settle, Record, Done };
Phase gPhase = Phase::WaitLoad;
size_t gState = 0;
Run gRun;
Json gRuns = Json::makeArray();
double gLoadedAt = 0;
std::string gPrevState = "visible";
std::string gRenderer;

Json num(double v) { return Json::makeNumber(v); }
Json str(const std::string& s) { return Json::makeString(s); }

void pushWindowEvent(SDL_Window* w, Uint8 ev) {
  if (!w) return;
  SDL_Event e{};
  e.type = SDL_WINDOWEVENT;
  e.window.windowID = SDL_GetWindowID(w);
  e.window.event = ev;
  SDL_PushEvent(&e);
}

// The events the window system sends for a change of state (a hidden run's window is never on screen).
void enterState(SDL_Window* w, const std::string& from, const std::string& to) {
  if (from == to) return;
  if (from == "minimised") pushWindowEvent(w, SDL_WINDOWEVENT_RESTORED);
  if (from == "hidden") pushWindowEvent(w, SDL_WINDOWEVENT_SHOWN), pushWindowEvent(w, SDL_WINDOWEVENT_EXPOSED);
  if (to == "visible") {
    pushWindowEvent(w, SDL_WINDOWEVENT_FOCUS_GAINED);
  } else {
    if (from == "visible") pushWindowEvent(w, SDL_WINDOWEVENT_FOCUS_LOST);
    if (to == "minimised") pushWindowEvent(w, SDL_WINDOWEVENT_MINIMIZED);
    if (to == "hidden") pushWindowEvent(w, SDL_WINDOWEVENT_HIDDEN);
  }
}

template <class F>
void eachNode(App& a, F&& f) {
  const auto& prog = a.engine.program();
  if (!prog) return;
  for (const ProgChannel& c : prog->channels)
    if (c.rt) f(NodeKey{0, c.id, -1}, c.rt.get(), static_cast<EffectRuntime*>(nullptr));
  for (const ProgMixer& m : prog->mixer)
    for (size_t k = 0; k < m.slots.size(); ++k)
      if (m.slots[k].rt) f(NodeKey{1, m.id, static_cast<int>(k)}, static_cast<ChannelRuntime*>(nullptr), m.slots[k].rt.get());
}

void beginRecord(App& a) {
  Run& r = gRun;
  // Counters start from zero (nothing counts them while counting is off).
  eachNode(a, [](const NodeKey& key, ChannelRuntime* c, EffectRuntime* e) {
    if (c) c->dspCalls.store(0, std::memory_order_relaxed), gRun.nodes0[key] = {c->restBlocks.load(), c->runBlocks.load()};
    if (e) e->dspCalls.store(0, std::memory_order_relaxed), gRun.nodes0[key] = {e->restBlocks.load(), 0};
  });
  for (auto& c : safefs::gCalls) c.store(0, std::memory_order_relaxed);
  gCallbacks.store(0, std::memory_order_relaxed);
  gCallbackCpuNs.store(0, std::memory_order_relaxed);
  r.graphBlocks0 = a.engine.serialBlocks() + a.engine.parallelBlocks();
  r.poolWakes0 = a.engine.workerWakeups();
  r.main0 = mainLoop();
  r.slept0 = a.engine.sleptBlocks();  // K-142 (follow-up)
  r.longRest0 = a.engine.longRestBlocks();
  r.counted0 = a.engine.countedBlocks();
  r.audible0 = a.engine.audibleBlocks();
  r.frames = 0;
  threads::setCountWakes(true);
  safefs::gCountCalls.store(true, std::memory_order_relaxed);
  a.engine.setCallCounting(true);
  gCounting.store(true, std::memory_order_relaxed);
  // The process is sampled around the threads (before them here, after them at the end), so the
  // threads' times fall within the process's and add up to no more than it.
  r.p0 = processSnap();
  r.t0 = threads::sampleThreads();
  r.start = nowSec();
  r.recording = true;
}

std::string songName(App& a, const NodeKey& k) {
  const Song& s = a.doc.song();
  if (k.kind == 0) {
    const Channel* c = s.channel(k.owner);
    return c ? c->name : std::string();
  }
  const MixerTrack* m = s.mixerTrack(k.owner);
  if (!m) return {};
  if (!m->name.empty()) return m->name;
  return k.owner == s.mixer.front().id ? "Master" : "Insert " + std::to_string(k.owner);
}

void endRecord(App& a) {
  Run& r = gRun;
  const std::vector<threads::ThreadTime> t1 = threads::sampleThreads();
  const double callbackCpu = static_cast<double>(gCallbackCpuNs.load(std::memory_order_relaxed)) * 1e-9;  // read with the threads
  const ProcessSnap p1 = processSnap();
  const double span = std::max(1e-6, nowSec() - r.start);
  gCounting.store(false, std::memory_order_relaxed);
  a.engine.setCallCounting(false);
  safefs::gCountCalls.store(false, std::memory_order_relaxed);
  threads::setCountWakes(false);  // (the counts stay readable until counting starts again)
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a block in flight finishes counting

  Json run = Json::makeObject();
  run.object["state"] = str(r.state);
  run.object["seconds"] = num(span);
  // Process.
  Json proc = Json::makeObject();
  const double cpu = p1.cpu - r.p0.cpu;
  proc.object["cpu_seconds"] = num(cpu);
  proc.object["cpu_percent_of_one_core"] = num(cpu / span * 100.0);
  proc.object["user_seconds"] = num(p1.user - r.p0.user);
  proc.object["system_seconds"] = num(p1.sys - r.p0.sys);
  proc.object["voluntary_switches_per_s"] = num(static_cast<double>(p1.voluntarySwitches - r.p0.voluntarySwitches) / span);
  proc.object["involuntary_switches_per_s"] = num(static_cast<double>(p1.involuntarySwitches - r.p0.involuntarySwitches) / span);
#if defined(__APPLE__)
  proc.object["syscalls_unix_per_s"] = num(static_cast<double>(p1.syscallsUnix - r.p0.syscallsUnix) / span);
  proc.object["syscalls_mach_per_s"] = num(static_cast<double>(p1.syscallsMach - r.p0.syscallsMach) / span);
  proc.object["context_switches_per_s"] = num(static_cast<double>(p1.machSwitches - r.p0.machSwitches) / span);
  proc.object["disk_read_bytes"] = num(static_cast<double>(p1.diskRead - r.p0.diskRead));
  proc.object["disk_written_bytes"] = num(static_cast<double>(p1.diskWritten - r.p0.diskWritten));
#endif
  run.object["process"] = proc;
  // Threads: by thread, and summed by name.
  std::map<uint64_t, double> before;
  for (const auto& t : r.t0) before[t.id] = t.cpu;
  Json ths = Json::makeArray();
  std::map<std::string, std::pair<double, int>> byName;
  std::vector<std::pair<double, const threads::ThreadTime*>> order;
  for (const auto& t : t1) {
    const auto it = before.find(t.id);
    const double d = std::max(0.0, t.cpu - (it != before.end() ? it->second : 0.0));
    order.push_back({d, &t});
    auto& bn = byName[t.name.empty() ? "(unnamed)" : t.name];
    bn.first += d;
    ++bn.second;
  }
  std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  for (const auto& [d, t] : order) {
    Json j = Json::makeObject();
    j.object["name"] = str(t->name.empty() ? "(unnamed)" : t->name);
    j.object["id"] = num(static_cast<double>(t->id));
    j.object["cpu_seconds"] = num(d);
    j.object["cpu_percent"] = num(d / span * 100.0);
    ths.array.push_back(j);
  }
  run.object["threads"] = ths;
  std::vector<std::pair<double, std::string>> names;
  for (const auto& [n, v] : byName) names.push_back({v.first, n});
  std::sort(names.begin(), names.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  Json tn = Json::makeArray();
  for (const auto& [d, n] : names) {
    Json j = Json::makeObject();
    j.object["name"] = str(n);
    j.object["count"] = num(byName[n].second);
    j.object["cpu_percent"] = num(d / span * 100.0);
    tn.array.push_back(j);
  }
  run.object["threads_by_name"] = tn;
  run.object["thread_count"] = num(static_cast<double>(t1.size()));
  // K-142z: Gridline's own threads and the system's (CpuReport.h). The budget is judged on "own".
  {
    double own = 0, sys = 0, audioThread = 0;
    std::map<std::string, double> ownBy, sysBy;
    for (const auto& [d, t] : order) {
      const std::string n = t->name.empty() ? "(unnamed)" : t->name;
      if (n == "gl-audio") audioThread += d;
      else if (n.rfind("gl-", 0) == 0) own += d, ownBy[n] += d;
      else sys += d, sysBy[n] += d;
    }
    const bool known = !t1.empty();
    // The audio device thread: Gridline's callback is own, the driver's waiting and waking is not.
    const double audioOwn = known ? std::min(callbackCpu, audioThread) : callbackCpu;
    const double audioDriver = known ? audioThread - audioOwn : 0.0;
    if (audioOwn > 0) ownBy["gl-audio (callback)"] += audioOwn;
    if (audioDriver > 0) sysBy["gl-audio (driver)"] += audioDriver;
    own += audioOwn;
    sys += audioDriver;
    // Threads that ended while recording are in the process total and in no thread's time; while idle
    // they are the system's short-lived workers (Gridline's own threads live as long as the app).
    double exited = std::max(0.0, cpu - own - sys);
    if (!known) own = cpu, sys = 0, exited = 0;  // no per-thread times here: the whole process counts as own
    sys += exited;
    auto list = [&](const std::map<std::string, double>& m) {
      std::vector<std::pair<double, std::string>> v;
      for (const auto& [n, d] : m) v.push_back({d, n});
      std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
      Json a = Json::makeArray();
      for (const auto& [d, n] : v) {
        Json j = Json::makeObject();
        j.object["name"] = str(n);
        j.object["cpu_percent"] = num(d / span * 100.0);
        a.array.push_back(j);
      }
      return a;
    };
    Json sp = Json::makeObject();
    sp.object["threads_known"] = Json::makeNumber(known ? 1 : 0);
    sp.object["total_percent"] = num(cpu / span * 100.0);
    sp.object["own_percent"] = num(own / span * 100.0);
    sp.object["system_percent"] = num(sys / span * 100.0);
    sp.object["exited_threads_percent"] = num(exited / span * 100.0);
    sp.object["audio_thread_percent"] = num(audioThread / span * 100.0);
    sp.object["audio_callback_percent"] = num(audioOwn / span * 100.0);
    sp.object["audio_driver_percent"] = num(audioDriver / span * 100.0);
    sp.object["main_frames_percent"] = num(r.frameCpuNs * 1e-9 / span * 100.0);  // drawing, inside gl-main's own time
    // Of that, the OpenGL calls that draw and present the frames: a hardware driver hands them to
    // the graphics card, a software one (gl_software) draws them right there on the processor.
    sp.object["main_gl_percent"] = num(r.glCpuNs * 1e-9 / span * 100.0);
    sp.object["own_threads"] = list(ownBy);
    sp.object["system_threads"] = list(sysBy);
    run.object["cpu_split"] = sp;
  }
  // Frames and audio.
  run.object["frames"] = num(r.frames);
  run.object["frames_per_s"] = num(r.frames / span);
  run.object["frame_cpu_ms"] = num(r.frames > 0 ? r.frameCpuNs / r.frames * 1e-6 : 0.0);  // per frame drawn
  Json au = Json::makeObject();
  au.object["device_callbacks"] = num(static_cast<double>(gCallbacks.load()));
  au.object["engine_blocks"] = num(static_cast<double>(a.engine.countedBlocks() - r.counted0));
  au.object["graph_blocks"] = num(static_cast<double>(a.engine.serialBlocks() + a.engine.parallelBlocks() - r.graphBlocks0));
  au.object["audible_blocks"] = num(static_cast<double>(a.engine.audibleBlocks() - r.audible0));
  au.object["pool_wakeups"] = num(static_cast<double>(a.engine.workerWakeups() - r.poolWakes0));
  // K-142 (0.7.8 follow-up): blocks the whole graph slept through, and long blocks at rest (AudioEngine::sleepGraph).
  au.object["slept_blocks"] = num(static_cast<double>(a.engine.sleptBlocks() - r.slept0));
  au.object["long_rest_blocks"] = num(static_cast<double>(a.engine.longRestBlocks() - r.longRest0));
  au.object["pool_threads"] = num(a.engine.workerThreads());
  au.object["sample_rate"] = num(a.engine.sampleRate());
  run.object["audio"] = au;
  // Instruments and effects.
  Json nodes = Json::makeArray();
  uint64_t dspTotal = 0;
  int running = 0;
  eachNode(a, [&](const NodeKey& key, ChannelRuntime* c, EffectRuntime* e) {
    Json j = Json::makeObject();
    const NodeStart n0 = gRun.nodes0.count(key) ? gRun.nodes0[key] : NodeStart{};
    j.object["kind"] = str(key.kind == 0 ? "instrument" : "effect");
    j.object["owner"] = num(static_cast<double>(key.owner));
    if (key.slot >= 0) j.object["slot"] = num(key.slot);
    j.object["name"] = str(songName(a, key));
    uint64_t calls = 0;
    if (c) {
      j.object["type"] = str(c->type);
      calls = c->dspCalls.load();
      j.object["rest_blocks"] = num(static_cast<double>(c->restBlocks.load() - n0.rest));
      j.object["run_blocks"] = num(static_cast<double>(c->runBlocks.load() - n0.run));
      j.object["resting"] = Json::makeNumber(c->restingUi.load() ? 1 : 0);
      j.object["sounding"] = Json::makeNumber(c->sounding.load() ? 1 : 0);
    } else {
      j.object["type"] = str(e->type);
      calls = e->dspCalls.load();
      j.object["rest_blocks"] = num(static_cast<double>(e->restBlocks.load() - n0.rest));
    }
    j.object["dsp_calls"] = num(static_cast<double>(calls));
    dspTotal += calls;
    if (calls > 0) ++running;
    nodes.array.push_back(j);
  });
  run.object["nodes"] = nodes;
  run.object["dsp_calls_total"] = num(static_cast<double>(dspTotal));
  run.object["nodes_running"] = num(running);
  // File system and workers.
  Json fsj = Json::makeObject();
  fsj.object["exists"] = num(static_cast<double>(safefs::gCalls[safefs::kFsExists].load()));
  fsj.object["is_file"] = num(static_cast<double>(safefs::gCalls[safefs::kFsIsFile].load()));
  fsj.object["is_dir"] = num(static_cast<double>(safefs::gCalls[safefs::kFsIsDir].load()));
  run.object["fs_calls"] = fsj;
  Json wk = Json::makeArray();
  for (const auto& w : threads::wakes()) {
    if (w.count == 0) continue;
    Json j = Json::makeObject();
    j.object["name"] = str(w.name);
    j.object["wakeups"] = num(static_cast<double>(w.count));
    j.object["per_s"] = num(static_cast<double>(w.count) / span);
    wk.array.push_back(j);
  }
  run.object["worker_wakeups"] = wk;
  {
    const MainLoop& m = mainLoop();
    const MainLoop& m0 = r.main0;
    auto rate = [&](uint64_t v, uint64_t v0) { return num(static_cast<double>(v - v0) / span); };
    Json ml = Json::makeObject();
    ml.object["sleeps_per_s"] = rate(m.sleeps, m0.sleeps);
    ml.object["timeouts_per_s"] = rate(m.timeouts, m0.timeouts);
    ml.object["events_per_s"] = rate(m.events, m0.events);
    ml.object["pokes_per_s"] = rate(m.pokes, m0.pokes);
    ml.object["busy_per_s"] = rate(m.busy, m0.busy);
    ml.object["turns_per_s"] = rate(m.turns, m0.turns);
    ml.object["wait_cpu_percent"] = num(static_cast<double>(m.waitCpuNs - m0.waitCpuNs) * 1e-9 / span * 100.0);
    run.object["main_loop"] = ml;
  }
  gRuns.array.push_back(run);
  r.recording = false;
}

std::string machine() {
#if defined(__APPLE__)
  char model[128] = "";
  size_t n = sizeof model;
  sysctlbyname("machdep.cpu.brand_string", model, &n, nullptr, 0);
  return model;
#elif !defined(_WIN32)
  utsname u{};
  if (uname(&u) == 0) return std::string(u.sysname) + " " + u.machine;
  return {};
#else
  return "Windows";
#endif
}

bool writeReport(App& a) {
  Json root = Json::makeObject();
  root.object["format"] = str("gridline-cpu-report-1");
  root.object["version"] = str(GRIDLINE_VERSION);
#if defined(NDEBUG)
  root.object["build"] = str("release");
#else
  root.object["build"] = str("debug");
#endif
  root.object["machine"] = str(machine());
  root.object["cores"] = num(std::thread::hardware_concurrency());
  root.object["layout"] = str(layout::id(layoutui::active()));
  root.object["project"] = str(a.projectFile());
  root.object["settle_seconds"] = num(gOpt.settle);
  root.object["record_seconds"] = num(gOpt.seconds);
  const char* drv = SDL_GetCurrentAudioDriver();
  root.object["audio_driver"] = str(drv ? drv : "");
  root.object["gl_renderer"] = str(gRenderer);
  {
    Json soft;
    soft.type = Json::Type::Bool;
    soft.boolean = softwareRenderer(gRenderer.c_str());
    root.object["gl_software"] = soft;
  }
  root.object["runs"] = gRuns;
  std::string path = gOpt.out;
  if (path.empty()) path = (std::filesystem::path(a.configDir()) / "cpu-report.json").string();
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cpu-report: cannot write %s\n", path.c_str());
    return false;
  }
  const std::string text = jsonStringify(root);
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  std::printf("cpu-report: written %s\n", path.c_str());
  std::fflush(stdout);
  return true;
}

}  // namespace

void configure(const Options& o) { gOpt = o; }
bool on() { return gOpt.seconds > 0; }

bool parseStates(const std::string& list, std::vector<std::string>& out) {
  out.clear();
  size_t at = 0;
  while (at <= list.size()) {
    const size_t c = list.find(',', at);
    std::string s = list.substr(at, c == std::string::npos ? std::string::npos : c - at);
    if (s == "minimized") s = "minimised";
    if (s != "visible" && s != "unfocused" && s != "minimised" && s != "hidden") return false;
    out.push_back(s);
    if (c == std::string::npos) break;
    at = c + 1;
  }
  return !out.empty();
}

void setRenderer(const char* renderer) { gRenderer = renderer ? renderer : ""; }

bool softwareRenderer(const char* renderer) {
  std::string low = renderer ? renderer : "";
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* k : {"llvmpipe", "softpipe", "swrast", "swiftshader", "software"})
    if (low.find(k) != std::string::npos) return true;
  return false;
}

bool frame(App& a, SDL_Window* window, bool drew, long long frameCpuNs, long long glCpuNs) {
  if (!on() || gPhase == Phase::Done) return false;
  const double now = nowSec();
  switch (gPhase) {
    case Phase::WaitLoad:
      if (a.projectLoading()) return false;
      gLoadedAt = now;
      gState = 0;
      gRun = Run{};
      gRun.state = gOpt.states[0];
      gRun.enter = now;
      enterState(window, gPrevState, gRun.state);
      gPrevState = gRun.state;
      std::printf("cpu-report: loaded; state %s, settling %.0f s\n", gRun.state.c_str(), gOpt.settle);
      std::fflush(stdout);
      gPhase = Phase::Settle;
      return false;
    case Phase::Settle:
      if (now - gRun.enter < gOpt.settle) return false;
      beginRecord(a);
      std::printf("cpu-report: recording %s for %.0f s\n", gRun.state.c_str(), gOpt.seconds);
      std::fflush(stdout);
      gPhase = Phase::Record;
      return false;
    case Phase::Record:
      // (The wake that ends the recording is not one of its frames: a hidden window draws none.)
      if (now - gRun.start < gOpt.seconds) {
        if (drew) ++gRun.frames, gRun.frameCpuNs += static_cast<double>(frameCpuNs), gRun.glCpuNs += static_cast<double>(glCpuNs);
        return false;
      }
      endRecord(a);
      {
        const Json& last = gRuns.array.back();
        const Json* p = last.find("process");
        std::printf("cpu-report: %s cpu %.2f%% frames/s %.1f dsp calls %.0f\n", gRun.state.c_str(), p ? p->num("cpu_percent_of_one_core") : 0.0,
                    last.num("frames_per_s"), last.num("dsp_calls_total"));
        std::fflush(stdout);
      }
      if (++gState < gOpt.states.size()) {
        const std::string next = gOpt.states[gState];
        gRun = Run{};
        gRun.state = next;
        gRun.enter = now;
        enterState(window, gPrevState, next);
        gPrevState = next;
        gPhase = Phase::Settle;
        return false;
      }
      writeReport(a);
      gPhase = Phase::Done;
      return true;
    case Phase::Done: return false;
  }
  return false;
}

double secondsToNext() {
  if (!on()) return 1e300;
  const double now = nowSec();
  switch (gPhase) {
    case Phase::Settle: return std::max(0.0, gRun.enter + gOpt.settle - now);
    case Phase::Record: return std::max(0.0, gRun.start + gOpt.seconds - now);
    case Phase::WaitLoad: return 0.25;
    case Phase::Done: return 1e300;
  }
  return 1e300;
}

}  // namespace gl::cpureport

// Gridline - K-142 (0.7.8): thread names and per-thread processor time (see ThreadName.h).
#include "ThreadName.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
#if defined(__OpenBSD__)
#include <pthread_np.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace gl::threads {

namespace {
// The name setName gave this thread, kept here too: not every system reads a thread's name back
// (the BSDs and Windows here do not), and the wakeup count groups threads by it.
thread_local char tName[16] = {};
}  // namespace

void setName(const char* name) noexcept {
  if (!name || !*name) return;
  char buf[16];
  std::strncpy(buf, name, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  std::memcpy(tName, buf, sizeof tName);
#if defined(__APPLE__)
  pthread_setname_np(buf);
#elif defined(__linux__) || defined(__FreeBSD__)
  pthread_setname_np(pthread_self(), buf);
#elif defined(__NetBSD__)
  pthread_setname_np(pthread_self(), "%s", static_cast<void*>(buf));
#elif defined(__OpenBSD__)
  pthread_set_name_np(pthread_self(), buf);
#elif defined(_WIN32)
  // SetThreadDescription: Windows 10 1607 and later (looked up, so older systems still start).
  using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const SetDesc set = [] {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    return k ? reinterpret_cast<SetDesc>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription"))) : nullptr;
  }();
  if (set) {
    wchar_t w[16];
    size_t i = 0;
    for (; buf[i] && i + 1 < 16; ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(buf[i]));
    w[i] = 0;
    set(GetCurrentThread(), w);
  }
#endif
}

std::string currentName() {
  if (tName[0]) return tName;
#if defined(__APPLE__) || defined(__linux__)
  char name[64] = "";
  pthread_getname_np(pthread_self(), name, sizeof name);
  return name;
#else
  return {};
#endif
}

// ---- wakeups ----
std::atomic<bool> gCountWakes{false};

namespace {
constexpr int kSlots = 256;
struct Slot {
  char name[32] = {};
  std::atomic<uint64_t> n{0};
};
Slot gSlots[kSlots];
std::atomic<int> gUsed{0};
thread_local int tSlot = -1;
}  // namespace

void wokeSlow() noexcept {
  if (tSlot < 0) {
    const int k = gUsed.fetch_add(1, std::memory_order_relaxed);
    if (k >= kSlots) {
      tSlot = kSlots;  // (full: this thread is not counted)
      return;
    }
    std::snprintf(gSlots[k].name, sizeof gSlots[k].name, "%s", currentName().c_str());
    if (!gSlots[k].name[0]) std::snprintf(gSlots[k].name, sizeof gSlots[k].name, "unnamed");
    tSlot = k;
  }
  if (tSlot < kSlots) gSlots[tSlot].n.fetch_add(1, std::memory_order_relaxed);
}

void setCountWakes(bool on) {
  if (on) {
    const int used = std::min(gUsed.load(), kSlots);
    for (int k = 0; k < used; ++k) gSlots[k].n.store(0, std::memory_order_relaxed);
  }
  gCountWakes.store(on, std::memory_order_relaxed);
}

std::vector<Wakes> wakes() {
  std::map<std::string, uint64_t> by;
  const int used = std::min(gUsed.load(), kSlots);
  for (int k = 0; k < used; ++k) by[gSlots[k].name] += gSlots[k].n.load(std::memory_order_relaxed);
  std::vector<Wakes> out;
  for (const auto& [name, n] : by) out.push_back({name, n});
  return out;
}

// ---- per-thread processor time ----
std::vector<ThreadTime> sampleThreads() {
  std::vector<ThreadTime> out;
#if defined(__APPLE__)
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return out;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_extended_info_data_t ext{};
    mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT;
    thread_identifier_info_data_t ident{};
    mach_msg_type_number_t ni = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(list[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&ext), &n) == KERN_SUCCESS) {
      ThreadTime t;
      if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &ni) == KERN_SUCCESS) t.id = ident.thread_id;
      ext.pth_name[sizeof ext.pth_name - 1] = 0;
      t.name = ext.pth_name;
      t.cpu = static_cast<double>(ext.pth_user_time + ext.pth_system_time) * 1e-9;
      out.push_back(std::move(t));
    }
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), sizeof(thread_t) * count);
#elif defined(__linux__)
  const double tick = static_cast<double>(sysconf(_SC_CLK_TCK));
  DIR* d = opendir("/proc/self/task");
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    ThreadTime t;
    t.id = std::strtoull(e->d_name, nullptr, 10);
    char path[96];
    std::snprintf(path, sizeof path, "/proc/self/task/%llu/stat", static_cast<unsigned long long>(t.id));
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    char buf[1024] = {};
    const size_t got = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[got] = 0;
    // pid (comm) state ... : the name is between the first '(' and the last ')'.
    const char* open = std::strchr(buf, '(');
    const char* close = std::strrchr(buf, ')');
    if (!open || !close || close < open) continue;
    t.name.assign(open + 1, close);
    // Fields after ')': state(3) ... utime(14) stime(15): the 12th and 13th after the state.
    const char* p = close + 2;
    unsigned long long ut = 0, st = 0;
    int field = 3;
    while (*p && field < 14) {
      if (*p == ' ') ++field;
      ++p;
    }
    if (std::sscanf(p, "%llu %llu", &ut, &st) == 2 && tick > 0) t.cpu = static_cast<double>(ut + st) / tick;
    // The stat file counts whole clock ticks (10 ms): over a few seconds of a thread that runs a
    // fraction of a percent, each thread's time was off by up to a tick (a third of a percent over
    // 3 s), and the threads' times no longer added up to the process's (which getrusage gives to
    // the microsecond). The thread's own processor-time clock is exact to the nanosecond and is the
    // one the process total sums: the kernel's clock id for a thread of this process is its id
    // inverted, shifted, with "per thread, scheduler time" (what pthread_getcpuclockid makes from
    // a pthread_t; this has the thread id, not the pthread_t). Kept the tick count where it fails.
    const unsigned tid = static_cast<unsigned>(t.id);
    const clockid_t clk = static_cast<clockid_t>((~tid << 3) | 6u);
    timespec ts{};
    if (clock_gettime(clk, &ts) == 0) t.cpu = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    out.push_back(std::move(t));
  }
  closedir(d);
#endif
  return out;
}

}  // namespace gl::threads

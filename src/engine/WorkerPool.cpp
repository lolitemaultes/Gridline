#include "WorkerPool.h"
#include "Denormals.h"
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#endif

namespace gl {

#if defined(__APPLE__)
Semaphore::Semaphore() : h_(dispatch_semaphore_create(0)) {}
Semaphore::~Semaphore() { dispatch_release(static_cast<dispatch_semaphore_t>(h_)); }
void Semaphore::post() { dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(h_)); }
void Semaphore::wait() { dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(h_), DISPATCH_TIME_FOREVER); }
#elif defined(_WIN32)
Semaphore::Semaphore() : h_(CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr)) {}
Semaphore::~Semaphore() { CloseHandle(static_cast<HANDLE>(h_)); }
void Semaphore::post() { ReleaseSemaphore(static_cast<HANDLE>(h_), 1, nullptr); }
void Semaphore::wait() { WaitForSingleObject(static_cast<HANDLE>(h_), INFINITE); }
#else
Semaphore::Semaphore() : h_(new sem_t) { sem_init(static_cast<sem_t*>(h_), 0, 0); }
Semaphore::~Semaphore() {
  sem_destroy(static_cast<sem_t*>(h_));
  delete static_cast<sem_t*>(h_);
}
void Semaphore::post() { sem_post(static_cast<sem_t*>(h_)); }
void Semaphore::wait() {
  while (sem_wait(static_cast<sem_t*>(h_)) != 0) {
  }
}
#endif

WorkerPool::~WorkerPool() {
  quit_.store(true);
  for (size_t i = 0; i < threads_.size(); ++i) sem_.post();
  for (auto& t : threads_) t.join();
}

void WorkerPool::ensureThreads(int count) {
  count = count < 0 ? 0 : count > kMaxThreads - 1 ? kMaxThreads - 1 : count;
  while (static_cast<int>(threads_.size()) < count) {
    int index = static_cast<int>(threads_.size()) + 1;
    threads_.emplace_back([this, index] { loop(index); });
  }
  spawned_.store(static_cast<int>(threads_.size()), std::memory_order_release);
}

namespace {
// Workers do part of every audio block, and the audio thread waits for the
// tasks they claimed: a worker the OS preempts for a scheduler quantum (10 ms)
// stalls the whole block. Run them with real-time scheduling like the audio
// thread itself (best effort: falls back to the default policy when refused).
bool makeRealtime() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  const double ticksPerMs = 1e6 * static_cast<double>(tb.denom) / static_cast<double>(tb.numer);
  thread_time_constraint_policy_data_t pol;
  pol.period = static_cast<uint32_t>(5.0 * ticksPerMs);       // ~ a 256-frame buffer
  pol.computation = static_cast<uint32_t>(2.5 * ticksPerMs);  // share of it a worker may use
  pol.constraint = static_cast<uint32_t>(5.0 * ticksPerMs);
  pol.preemptible = 1;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                           reinterpret_cast<thread_policy_t>(&pol), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#elif defined(_WIN32)
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
  sched_param sp{};
  sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 10;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;  // needs rtprio (e.g. audio group)
#endif
}
}  // namespace

void WorkerPool::loop(int index) {
  (void)index;
  flushDenormals();  // same float mode as the audio thread (identical output for any thread count)
  if (makeRealtime()) realtime_.fetch_add(1);
  for (;;) {
    sem_.wait();
    if (quit_.load()) return;
    // Dekker-style handshake with run(): either this thread sees the job
    // closed, or run() sees it inside and waits for it to leave.
    inside_.fetch_add(1);
    if (open_.load()) {
      JobFn fn = fn_.load(std::memory_order_acquire);
      void* ctx = ctx_.load(std::memory_order_acquire);
      int w = nextWorker_.fetch_add(1);
      if (fn && w < kMaxThreads) fn(ctx, w);
    }
    inside_.fetch_sub(1);
  }
}

void WorkerPool::run(JobFn fn, void* ctx, int workers) {
  const int n = workers < 0 ? 0 : workers > threads() ? threads() : workers;
  fn_.store(fn, std::memory_order_release);
  ctx_.store(ctx, std::memory_order_release);
  nextWorker_.store(1);
  open_.store(true);
  for (int i = 0; i < n; ++i) sem_.post();
  fn(ctx, 0);
  open_.store(false);
  while (inside_.load() != 0) std::this_thread::yield();
}

}  // namespace gl

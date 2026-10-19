// Gridline — worker threads for the parallel mixer graph. The audio thread
// publishes a job (a function over task indices), wakes some workers through a
// semaphore (posting never blocks) and works on the job itself; tasks are
// handed out through atomics only (no locks on the audio thread). Workers
// sleep on the semaphore between blocks.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace gl {

// Counting semaphore (dispatch_semaphore on macOS, POSIX sem_t elsewhere).
class Semaphore {
 public:
  Semaphore();
  ~Semaphore();
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;
  void post();  // never blocks
  void wait();

 private:
  void* h_ = nullptr;
};

class WorkerPool {
 public:
  static constexpr int kMaxThreads = 16;  // including the audio thread

  // Runs `run(ctx, worker)` on up to `workers` pool threads plus the calling
  // thread (worker 0) until `run` returns; returns when every engaged pool
  // thread has left the job. `run` must itself return only when the job is
  // complete from its point of view (see AudioEngine's task claiming).
  using JobFn = void (*)(void* ctx, int worker);

  WorkerPool() = default;
  ~WorkerPool();
  // UI thread: makes sure `count` pool threads exist (never shrinks).
  void ensureThreads(int count);
  int threads() const { return spawned_.load(std::memory_order_acquire); }
  // Pool threads running with real-time scheduling (diagnostics / tests).
  int realtimeThreads() const { return realtime_.load(std::memory_order_acquire); }

  // Audio thread.
  void run(JobFn fn, void* ctx, int workers);

 private:
  void loop(int index);
  std::vector<std::thread> threads_;
  std::atomic<int> spawned_{0};
  std::atomic<int> realtime_{0};
  Semaphore sem_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> open_{false};
  std::atomic<int> inside_{0};
  std::atomic<JobFn> fn_{nullptr};
  std::atomic<void*> ctx_{nullptr};
  std::atomic<int> nextWorker_{1};
};

}  // namespace gl

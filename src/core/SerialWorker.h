// Gridline - FRZ-1118 (0.7.8): one background thread that runs jobs in the order they were posted. For file
// work on a web or network drive (RemoteFs.h) that the interface thread must never wait for: a
// mount that stops answering holds the writing thread in the kernel, for minutes.
//
// post() never blocks. drain(ms) waits at most `ms` for the jobs posted so far. The destructor
// drains for a short while and then leaves a job that is stuck behind (its thread keeps only the
// worker's own shared state alive, so nothing it touches goes away under it).
#pragma once
#include <atomic>
#include "ThreadName.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace gl {

class SerialWorker {
 public:
  SerialWorker() : s_(std::make_shared<State>()) {
    std::shared_ptr<State> s = s_;
    t_ = std::thread([s] { gl::threads::setName("gl-serial"); run(*s); });
  }
  ~SerialWorker() {
    drain(kLeaveWaitMs);
    bool idle;
    {
      std::lock_guard<std::mutex> lk(s_->mu);
      s_->stop = true;
      idle = !s_->busy;
    }
    s_->cv.notify_all();
    if (idle) {
      t_.join();
    } else {
      t_.detach();  // stuck in a job: it ends on its own (or with the process)
    }
  }
  SerialWorker(const SerialWorker&) = delete;
  SerialWorker& operator=(const SerialWorker&) = delete;

  void post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lk(s_->mu);
      s_->q.push_back(std::move(job));
    }
    s_->cv.notify_all();
  }
  // Waits at most `ms` until every job posted so far has run. True when they all have.
  bool drain(int ms) {
    std::unique_lock<std::mutex> lk(s_->mu);
    return s_->idle.wait_for(lk, std::chrono::milliseconds(ms), [&] { return s_->q.empty() && !s_->busy; });
  }
  // Jobs waiting or running.
  size_t pending() const {
    std::lock_guard<std::mutex> lk(s_->mu);
    return s_->q.size() + (s_->busy ? 1 : 0);
  }
  // Tests: every job first waits this long (a drive that answers slowly).
  static void setTestDelayMs(int ms) { testDelay().store(ms); }

  static constexpr int kLeaveWaitMs = 1500;

 private:
  struct State {
    mutable std::mutex mu;
    std::condition_variable cv, idle;
    std::deque<std::function<void()>> q;
    bool stop = false, busy = false;
  };
  static std::atomic<int>& testDelay() {
    static std::atomic<int> ms{0};
    return ms;
  }
  static void run(State& s) {
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
      s.cv.wait(lk, [&] { return s.stop || !s.q.empty(); });
      gl::threads::woke();  // K-142: CPU report
      if (s.q.empty()) return;  // (stop, nothing left)
      std::function<void()> job = std::move(s.q.front());
      s.q.pop_front();
      s.busy = true;
      lk.unlock();
      if (const int d = testDelay().load(); d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
      try {
        job();
      } catch (...) {  // a failed write: the next one tries again
      }
      lk.lock();
      s.busy = false;
      s.idle.notify_all();
    }
  }
  std::shared_ptr<State> s_;
  std::thread t_;
};

}  // namespace gl

// Gridline — opening the system MIDI service without trusting it (see MidiOpen.h).
#include "MidiOpen.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace gl {

const char* midiOpenStateName(MidiOpenState s) {
  switch (s) {
    case MidiOpenState::Idle: return "idle";
    case MidiOpenState::Opening: return "opening";
    case MidiOpenState::Ready: return "ready";
    case MidiOpenState::Failed: return "failed";
    case MidiOpenState::NotResponding: return "not-responding";
  }
  return "?";
}

// One probe's mailbox, shared with its thread: the thread may outlive the opener (a hung
// service), so it owns a reference and the opener only ever reads it under the lock.
struct MidiOpener::Shared {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  MidiProbeResult result;
};

MidiOpener::MidiOpener(Probe probe, double timeoutSec, std::vector<double> retryDelays)
    : probe_(std::move(probe)), timeout_(std::max(0.05, timeoutSec)), delays_(std::move(retryDelays)) {
  if (delays_.empty()) delays_.push_back(5.0);
}

MidiOpener::~MidiOpener() = default;  // a probe still out finishes (or hangs) on its own

void MidiOpener::launch(double now) {
  auto sh = std::make_shared<Shared>();
  ++attempts_;
  startedAt_ = now;
  retryAt_ = -1;
  state_ = MidiOpenState::Opening;
  try {
    std::thread([sh, probe = probe_] {
      MidiProbeResult r;
      try {
        r = probe();
      } catch (const std::exception& e) {
        r = MidiProbeResult{};
        r.error = e.what();
      } catch (...) {
        r = MidiProbeResult{};
        r.error = "unknown error";
      }
      if (!r.ok && r.error.empty()) r.error = "unknown error";
      std::lock_guard<std::mutex> lk(sh->mu);
      sh->result = std::move(r);
      sh->done = true;
      sh->cv.notify_all();
    }).detach();
    cur_ = std::move(sh);
  } catch (const std::exception& e) {  // no thread could be started
    cur_.reset();
    state_ = MidiOpenState::Failed;
    error_ = e.what();
    schedule(now);
  }
}

void MidiOpener::schedule(double now) {
  const double d = delays_[std::min(static_cast<size_t>(failures_), delays_.size() - 1)];
  ++failures_;
  retryAt_ = now + d;
}

void MidiOpener::start(double now) {
  if (cur_) return;
  launch(now);
}

void MidiOpener::retryNow(double now) {
  failures_ = 0;
  if (cur_) {  // still out (hung): keep waiting on it, never a second one beside it
    if (state_ != MidiOpenState::Opening) retryAt_ = now + delays_[0];
    return;
  }
  launch(now);
}

bool MidiOpener::probeRunning() const {
  if (!cur_) return false;
  std::lock_guard<std::mutex> lk(cur_->mu);
  return !cur_->done;
}

bool MidiOpener::update(double now) {
  if (cur_) {
    bool done = false;
    MidiProbeResult r;
    {
      std::lock_guard<std::mutex> lk(cur_->mu);
      if (cur_->done) {
        done = true;
        r = std::move(cur_->result);
      }
    }
    if (done) {
      cur_.reset();
      if (r.ok) {
        state_ = MidiOpenState::Ready;
        error_.clear();
        failures_ = 0;
        retryAt_ = -1;
      } else {
        state_ = MidiOpenState::Failed;
        error_ = r.error;
        schedule(now);
      }
      result_ = std::move(r);
      return true;
    }
    if (state_ == MidiOpenState::Opening && now - startedAt_ >= timeout_) {
      state_ = MidiOpenState::NotResponding;
      schedule(now);
    }
  }
  if ((state_ == MidiOpenState::Failed || state_ == MidiOpenState::NotResponding) && retryAt_ >= 0 && now >= retryAt_) {
    if (cur_)
      schedule(now);  // the hung probe is still out: wait on it longer
    else
      launch(now);
  }
  return false;
}

bool MidiOpener::waitFor(double seconds) {
  if (!cur_) return false;
  std::shared_ptr<Shared> sh = cur_;
  std::unique_lock<std::mutex> lk(sh->mu);
  return sh->cv.wait_for(lk, std::chrono::duration<double>(std::max(0.0, seconds)), [&] { return sh->done; });
}

double MidiOpener::nextRetryIn(double now) const { return retryAt_ < 0 ? -1.0 : std::max(0.0, retryAt_ - now); }

MidiProbeResult MidiOpener::takeResult() {
  MidiProbeResult r = std::move(result_);
  result_ = MidiProbeResult{};
  return r;
}

}  // namespace gl

#include "EditorClose.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "Backend.h"

namespace gl::host {

struct EditorClose::Shared {
  std::mutex lock;
  std::condition_variable cv;
  bool done = false, failed = false;
};

EditorClose::EditorClose(Editor* e) : s_(std::make_shared<Shared>()) {
  std::thread([s = s_, e]() {
    bool threw = false;
    try {
      e->detachOffThread();
    } catch (...) {
      threw = true;
    }
    std::lock_guard<std::mutex> g(s->lock);
    s->failed = threw;
    s->done = true;
    s->cv.notify_all();
  }).detach();
}

bool EditorClose::waitFor(int ms) {
  std::unique_lock<std::mutex> g(s_->lock);
  return s_->cv.wait_for(g, std::chrono::milliseconds(ms), [this] { return s_->done; });
}

bool EditorClose::done() const {
  std::lock_guard<std::mutex> g(s_->lock);
  return s_->done;
}

bool EditorClose::failed() const {
  std::lock_guard<std::mutex> g(s_->lock);
  return s_->failed;
}

}  // namespace gl::host

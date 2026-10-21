#include "Vst3Editor.h"
#include <algorithm>
#include <chrono>
#if !defined(_WIN32)
#include <poll.h>
#endif

namespace gl::vst3 {

namespace {
double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

sb::tresult PLUGIN_API RunLoop::registerEventHandler(sb::Linux::IEventHandler* handler, sb::Linux::FileDescriptor fd) {
  if (!handler) return sb::kInvalidArgument;
  fds_.push_back({handler, fd});
  return sb::kResultOk;
}

sb::tresult PLUGIN_API RunLoop::unregisterEventHandler(sb::Linux::IEventHandler* handler) {
  fds_.erase(std::remove_if(fds_.begin(), fds_.end(), [&](const Fd& f) { return f.handler == handler; }), fds_.end());
  return sb::kResultOk;
}

sb::tresult PLUGIN_API RunLoop::registerTimer(sb::Linux::ITimerHandler* handler, sb::Linux::TimerInterval ms) {
  if (!handler) return sb::kInvalidArgument;
  double iv = std::max<double>(1.0, static_cast<double>(ms)) / 1000.0;
  timers_.push_back({handler, iv, nowSeconds() + iv});
  return sb::kResultOk;
}

sb::tresult PLUGIN_API RunLoop::unregisterTimer(sb::Linux::ITimerHandler* handler) {
  timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.handler == handler; }), timers_.end());
  return sb::kResultOk;
}

void RunLoop::pump() {
#if !defined(_WIN32)
  if (!fds_.empty()) {
    std::vector<pollfd> p;
    for (const auto& f : fds_) p.push_back({f.fd, POLLIN, 0});
    if (poll(p.data(), static_cast<nfds_t>(p.size()), 0) > 0) {
      std::vector<Fd> ready;
      for (size_t i = 0; i < p.size(); ++i)
        if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(fds_[i]);
      for (const auto& f : ready)  // handlers may unregister: re-check membership
        if (std::any_of(fds_.begin(), fds_.end(), [&](const Fd& g) { return g.handler == f.handler && g.fd == f.fd; }))
          f.handler->onFDIsSet(f.fd);
    }
  }
#endif
  double now = nowSeconds();
  std::vector<sb::Linux::ITimerHandler*> due;
  for (auto& t : timers_)
    if (now >= t.due) {
      due.push_back(t.handler);
      t.due = now + t.interval;
    }
  for (auto* h : due)
    if (std::any_of(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.handler == h; })) h->onTimer();
}

RunLoop* runLoop() {
  static RunLoop* r = new RunLoop;  // never destroyed
  return r;
}

sb::tresult PLUGIN_API PlugFrame::resizeView(sb::IPlugView* view, sb::ViewRect* newSize) {
  if (!view || !newSize) return sb::kInvalidArgument;
  return onResize && onResize(view, newSize) ? sb::kResultOk : sb::kResultFalse;
}

sb::tresult PlugFrame::queryExtra(const sb::TUID iid, void** obj) {
#if !defined(__APPLE__) && !defined(_WIN32)  // X11 (Linux, the BSDs)
  if (std::memcmp(iid, sb::Linux::IRunLoop::iid.toTUID(), sizeof(sb::TUID)) == 0) {
    RunLoop* r = runLoop();
    r->addRef();
    *obj = static_cast<sb::Linux::IRunLoop*>(r);
    return sb::kResultOk;
  }
#else
  (void)iid;
#endif
  *obj = nullptr;
  return sb::kNoInterface;
}

const char* platformViewType() {
#if defined(__APPLE__)
  return sb::kPlatformTypeNSView;
#elif defined(_WIN32)
  return sb::kPlatformTypeHWND;
#else
  return sb::kPlatformTypeX11EmbedWindowID;
#endif
}

}  // namespace gl::vst3

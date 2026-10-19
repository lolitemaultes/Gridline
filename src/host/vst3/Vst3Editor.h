// Gridline — VST3 hosting: host side of plugin editors (IPlugFrame) and the
// Linux run loop plugins use for their UI timers and X11 connections.
// Windowing itself (SDL child windows, native handles) lives in the app.
#pragma once
#include <functional>
#include <vector>
#include "Vst3Common.h"

namespace gl::vst3 {

// Process-wide run loop (Linux IRunLoop): file descriptors and timers that
// plugin editors register; the UI thread calls pump() every frame.
class RunLoop final : public HostObject<sb::Linux::IRunLoop> {
 public:
  sb::tresult PLUGIN_API registerEventHandler(sb::Linux::IEventHandler* handler, sb::Linux::FileDescriptor fd) override;
  sb::tresult PLUGIN_API unregisterEventHandler(sb::Linux::IEventHandler* handler) override;
  sb::tresult PLUGIN_API registerTimer(sb::Linux::ITimerHandler* handler, sb::Linux::TimerInterval milliseconds) override;
  sb::tresult PLUGIN_API unregisterTimer(sb::Linux::ITimerHandler* handler) override;
  // Dispatches ready descriptors and due timers (never blocks).
  void pump();

 private:
  struct Fd {
    sb::Linux::IEventHandler* handler;
    int fd;
  };
  struct Timer {
    sb::Linux::ITimerHandler* handler;
    double interval, due;  // seconds
  };
  std::vector<Fd> fds_;
  std::vector<Timer> timers_;
};
RunLoop* runLoop();

// The frame an editor view lives in. `onResize` applies a size the plugin asks
// for (resize the native window, then call view->onSize).
class PlugFrame final : public HostObject<sb::IPlugFrame> {
 public:
  std::function<bool(sb::IPlugView*, sb::ViewRect*)> onResize;
  sb::tresult PLUGIN_API resizeView(sb::IPlugView* view, sb::ViewRect* newSize) override;

 protected:
  sb::tresult queryExtra(const sb::TUID iid, void** obj) override;
};

// Platform type string for IPlugView::attached ("NSView", "X11EmbedWindowID").
const char* platformViewType();

}  // namespace gl::vst3

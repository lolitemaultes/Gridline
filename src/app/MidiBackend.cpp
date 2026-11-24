// Gridline — MIDI back ends (see MidiBackend.h). On the BSDs, RtMidi is built with its dummy
// API and the sndio classes below take its place behind RtMidiIn / RtMidiOut.
#include "MidiBackend.h"
#include "../core/ThreadName.h"
#include "UiWake.h"
#include "../../third_party/rtmidi/RtMidi.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__) && defined(__LINUX_ALSA__)
#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <set>
#endif
#if defined(__MACOSX_CORE__) && !defined(GRIDLINE_MIDI_SNDIO)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
// RtMidi (CoreMIDI): every RtMidiIn / RtMidiOut shares one MIDI client; this sets it.
void RtMidi_setCoreMidiClientSingleton(MIDIClientRef client);
#endif

#if defined(GRIDLINE_MIDI_SNDIO)
#include <fcntl.h>
#include <poll.h>
#include <sndio.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>
#endif

namespace gl {

#if defined(GRIDLINE_MIDI_SNDIO)

namespace {

// sndio has no device listing: the MIDI thru box of sndiod and the raw MIDI ports that open.
std::vector<std::string> sndioPorts(unsigned mode) {
  std::vector<std::string> v;
  std::vector<std::string> cands = {"midithru/0"};
  for (int i = 0; i < 8; ++i) cands.push_back("rmidi/" + std::to_string(i));
  for (const auto& c : cands)
    if (mio_hdl* h = mio_open(c.c_str(), mode, 1)) {
      mio_close(h);
      v.push_back(c);
    }
  return v;
}

class MidiInSndio : public MidiInApi {
 public:
  MidiInSndio() : MidiInApi(100) { initialize("Gridline"); }
  ~MidiInSndio() override { closePort(); }
  RtMidi::Api getCurrentApi() override { return RtMidi::RTMIDI_DUMMY; }
  void openPort(unsigned int portNumber, const std::string&) override {
    closePort();
    const auto ports = sndioPorts(MIO_IN);
    if (portNumber >= ports.size()) {
      error(RtMidiError::INVALID_PARAMETER, "sndio: no such MIDI input");
      return;
    }
    hdl_ = mio_open(ports[portNumber].c_str(), MIO_IN, 1);
    if (!hdl_) {
      error(RtMidiError::DRIVER_ERROR, "sndio: cannot open " + ports[portNumber]);
      return;
    }
    connected_ = true;
    quit_ = false;
    // K-142 (0.7.8): the thread sleeps until the device sends or the port closes (a byte on this pipe)
    if (::pipe(wake_) == 0) {
      ::fcntl(wake_[0], F_SETFL, O_NONBLOCK);
      ::fcntl(wake_[0], F_SETFD, FD_CLOEXEC);
      ::fcntl(wake_[1], F_SETFD, FD_CLOEXEC);
    } else {
      wake_[0] = wake_[1] = -1;
    }
    thread_ = std::thread([this] { gl::threads::setName("gl-midi-in"); run(); });
  }
  void openVirtualPort(const std::string&) override { error(RtMidiError::WARNING, "sndio: virtual ports are not supported"); }
  void closePort() override {
    quit_ = true;
    if (wake_[1] >= 0) {
      const char b = 0;
      (void)!::write(wake_[1], &b, 1);
    }
    if (thread_.joinable()) thread_.join();
    for (int& f : wake_) {
      if (f >= 0) ::close(f);
      f = -1;
    }
    if (hdl_) mio_close(hdl_);
    hdl_ = nullptr;
    connected_ = false;
  }
  void setClientName(const std::string&) override {}
  void setPortName(const std::string&) override {}
  unsigned int getPortCount() override { return static_cast<unsigned>(sndioPorts(MIO_IN).size()); }
  std::string getPortName(unsigned int n) override {
    const auto p = sndioPorts(MIO_IN);
    return n < p.size() ? p[n] : std::string();
  }

 protected:
  void initialize(const std::string&) override {}

 private:
  void deliver() {
    const unsigned char s = msg_.empty() ? 0 : msg_[0];
    if ((s == 0xF0 && (inputData_.ignoreFlags & 0x01)) || ((s == 0xF1 || s == 0xF8) && (inputData_.ignoreFlags & 0x02)) ||
        (s == 0xFE && (inputData_.ignoreFlags & 0x04))) {
      msg_.clear();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double delta = first_ ? 0.0 : std::chrono::duration<double>(now - last_).count();
    first_ = false;
    last_ = now;
    if (inputData_.usingCallback && inputData_.userCallback) {
      inputData_.userCallback(delta, &msg_, inputData_.userData);
    } else {
      MidiMessage m;
      m.bytes = msg_;
      m.timeStamp = delta;
      inputData_.queue.push(m);
    }
    msg_.clear();
  }
  static int dataBytes(unsigned char status) {
    switch (status & 0xF0) {
      case 0xC0:
      case 0xD0: return 1;
      case 0xF0:
        return status == 0xF2 ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
      default: return 2;
    }
  }
  void byte(unsigned char b) {
    if (b >= 0xF8) {  // real-time: a message of its own, even inside another one
      std::vector<unsigned char> keep;
      keep.swap(msg_);
      msg_.push_back(b);
      deliver();
      msg_.swap(keep);
      return;
    }
    if (sysex_) {
      msg_.push_back(b);
      if (b == 0xF7) {
        sysex_ = false;
        deliver();
      }
      return;
    }
    if (b & 0x80) {
      msg_.clear();
      if (b == 0xF0) {
        sysex_ = true;
        msg_.push_back(b);
        return;
      }
      running_ = b < 0xF0 ? b : 0;
      msg_.push_back(b);
      need_ = dataBytes(b);
      if (need_ == 0) deliver();
      return;
    }
    if (msg_.empty()) {  // running status
      if (!running_) return;
      msg_.push_back(running_);
      need_ = dataBytes(running_);
    }
    msg_.push_back(b);
    if (static_cast<int>(msg_.size()) - 1 >= need_) deliver();
  }
  void run() {
    unsigned char buf[256];
    std::vector<pollfd> fds(static_cast<size_t>(mio_nfds(hdl_) > 0 ? mio_nfds(hdl_) : 1) + 1);
    while (!quit_) {
      const int n = mio_pollfd(hdl_, fds.data(), POLLIN);
      int count = n;
      if (wake_[0] >= 0) {
        fds[static_cast<size_t>(n)] = pollfd{wake_[0], POLLIN, 0};
        ++count;
      }
      // no wake pipe: look every 50 ms whether the port closed (as before)
      if (::poll(fds.data(), static_cast<nfds_t>(count), wake_[0] >= 0 ? -1 : 50) < 0) continue;
      gl::threads::woke();  // K-142: CPU report
      if (quit_) break;
      if (!(mio_revents(hdl_, fds.data()) & POLLIN)) {
        if (mio_eof(hdl_)) break;
        continue;
      }
      const size_t got = mio_read(hdl_, buf, sizeof buf);
      for (size_t i = 0; i < got; ++i) byte(buf[i]);
      if (mio_eof(hdl_)) break;
    }
  }
  mio_hdl* hdl_ = nullptr;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::vector<unsigned char> msg_;
  unsigned char running_ = 0;
  int need_ = 0;
  bool sysex_ = false, first_ = true;
  std::chrono::steady_clock::time_point last_;
};

class MidiOutSndio : public MidiOutApi {
 public:
  MidiOutSndio() { initialize("Gridline"); }
  ~MidiOutSndio() override { closePort(); }
  RtMidi::Api getCurrentApi() override { return RtMidi::RTMIDI_DUMMY; }
  void openPort(unsigned int portNumber, const std::string&) override {
    closePort();
    const auto ports = sndioPorts(MIO_OUT);
    if (portNumber >= ports.size()) {
      error(RtMidiError::INVALID_PARAMETER, "sndio: no such MIDI output");
      return;
    }
    hdl_ = mio_open(ports[portNumber].c_str(), MIO_OUT, 0);
    if (!hdl_) {
      error(RtMidiError::DRIVER_ERROR, "sndio: cannot open " + ports[portNumber]);
      return;
    }
    connected_ = true;
  }
  void openVirtualPort(const std::string&) override { error(RtMidiError::WARNING, "sndio: virtual ports are not supported"); }
  void closePort() override {
    if (hdl_) mio_close(hdl_);
    hdl_ = nullptr;
    connected_ = false;
  }
  void setClientName(const std::string&) override {}
  void setPortName(const std::string&) override {}
  unsigned int getPortCount() override { return static_cast<unsigned>(sndioPorts(MIO_OUT).size()); }
  std::string getPortName(unsigned int n) override {
    const auto p = sndioPorts(MIO_OUT);
    return n < p.size() ? p[n] : std::string();
  }
  void sendMessage(const unsigned char* message, size_t size) override {
    if (hdl_ && size > 0) mio_write(hdl_, message, size);
  }

 protected:
  void initialize(const std::string&) override {}

 private:
  mio_hdl* hdl_ = nullptr;
};

// RtMidiIn / RtMidiOut whose implementation is sndio: built on the dummy API, which is then
// replaced (rtapi_ is RtMidi's protected implementation pointer). The dummy's "provides no
// functionality" notice on std::cerr is muted while it exists (CerrMute is constructed first).
struct CerrMute {
  std::streambuf* old;
  CerrMute() : old(std::cerr.rdbuf(nullptr)) {}
  void restore() {
    std::cerr.rdbuf(old);
    std::cerr.clear();
  }
};
class SndioIn : private CerrMute, public RtMidiIn {
 public:
  SndioIn() : CerrMute(), RtMidiIn(RtMidi::RTMIDI_DUMMY, "Gridline") {
    restore();
    delete rtapi_;
    rtapi_ = new MidiInSndio;
  }
};
class SndioOut : private CerrMute, public RtMidiOut {
 public:
  SndioOut() : CerrMute(), RtMidiOut(RtMidi::RTMIDI_DUMMY, "Gridline") {
    restore();
    delete rtapi_;
    rtapi_ = new MidiOutSndio;
  }
};

}  // namespace

std::unique_ptr<RtMidiIn> makeMidiIn() { return std::make_unique<SndioIn>(); }
std::unique_ptr<RtMidiOut> makeMidiOut() { return std::make_unique<SndioOut>(); }
std::string midiBackendName() { return "sndio"; }

#else

std::unique_ptr<RtMidiIn> makeMidiIn() { return std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "Gridline"); }
std::unique_ptr<RtMidiOut> makeMidiOut() { return std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "Gridline"); }
std::string midiBackendName() {
#if defined(__MACOSX_CORE__)
  return "CoreMIDI";
#elif defined(__WINDOWS_MM__)
  return "Windows Multimedia";
#elif defined(__LINUX_ALSA__) && defined(__UNIX_JACK__)
  return "ALSA + JACK";
#elif defined(__LINUX_ALSA__)
  return "ALSA";
#elif defined(__UNIX_JACK__)
  return "JACK";
#else
  return "none (Gridline was built without a MIDI back end)";
#endif
}

#endif

namespace {

struct FakePorts {
  std::mutex mu;
  std::vector<std::string> ins, outs;
};
FakePorts& fakePorts() {
  static FakePorts f;
  return f;
}

std::string testMode() {
  const char* test = std::getenv("GRIDLINE_TEST_MIDI");
  return test ? test : "";
}

#if defined(__MACOSX_CORE__) && !defined(GRIDLINE_MIDI_SNDIO)
// MIDIHP-1119: the CoreMIDI client every RtMidi object shares is made on a thread of its own
// that keeps running its run loop. CoreMIDI brings a client its news of devices coming and
// going through the run loop of the thread that made it; RtMidi made it on the first probe's
// thread, which ends, so the client never heard of a device plugged in later. Here the news
// keeps coming, and each "setup changed" notification also asks Gridline to compare its
// device lists at once.
struct CoreWatch {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, ok = false;
  std::atomic<uint64_t> changes{0};
};
CoreWatch& coreWatch() {
  static CoreWatch* w = new CoreWatch;  // never freed: the watcher thread lives until the process ends
  return *w;
}
void coreNotify(const MIDINotification* n, void*) {
  if (n && (n->messageID == kMIDIMsgSetupChanged || n->messageID == kMIDIMsgObjectAdded || n->messageID == kMIDIMsgObjectRemoved)) {
    coreWatch().changes.fetch_add(1, std::memory_order_relaxed);
    uiwake::poke();  // K-142 (0.7.8): a hidden Gridline looks at once
  }
}
void keepAlive(CFRunLoopTimerRef, void*) {}
// Starts the watcher once and waits for its client (a hung MIDI server hangs this probe, which
// MidiOpener times out; the watcher then makes the client whenever the server answers).
void ensureCoreWatch() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] { gl::threads::setName("gl-midi-watch");
      CoreWatch& w = coreWatch();
      MIDIClientRef client = 0;
      const OSStatus st = MIDIClientCreate(CFSTR("Gridline"), coreNotify, nullptr, &client);
      const bool ok = st == noErr && client != 0;
      {
        std::lock_guard<std::mutex> lk(w.mu);
        w.done = true;
        w.ok = ok;
        if (ok) {
          RtMidi_setCoreMidiClientSingleton(client);
        }
      }
      w.cv.notify_all();
      if (!ok) {
        return;  // RtMidi makes its own client, as before
      }
      // The run loop needs a source to keep running: a timer that never matters.
      CFRunLoopTimerRef t = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 3600.0, 3600.0, 0, 0, keepAlive, nullptr);
      CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
      for (;;) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3600.0, false);
      }
    }).detach();
  });
  CoreWatch& w = coreWatch();
  std::unique_lock<std::mutex> lk(w.mu);
  w.cv.wait(lk, [&] { return w.done; });
}
#endif

#if defined(__linux__) && defined(__LINUX_ALSA__)
// MIDIHP-1119: a MIDI device plugged in or out adds or removes its nodes in /dev/snd (a USB
// keyboard: controlC2, midiC2D0). A thread of its own compares that folder's names (no MIDI
// client: every probe makes a sequencer client, which other programs see come and go), and a
// change asks Gridline to compare its device lists at once. K-142 (0.7.8): it sleeps until the
// system says the folder changed (inotify), and looks once a minute anyway; where the folder
// cannot be watched it looks every second, as before.
struct NodeWatch {
  std::atomic<uint64_t> changes{0};
  std::atomic<bool> ok{false};
};
NodeWatch& nodeWatch() {
  static NodeWatch* w = new NodeWatch;  // never freed: the watcher thread lives until the process ends
  return *w;
}
bool sndNodes(std::set<std::string>& out) {
  out.clear();
  DIR* d = opendir("/dev/snd");
  if (!d) {
    return false;
  }
  while (const dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.rfind("midiC", 0) == 0 || n.rfind("controlC", 0) == 0) {
      out.insert(n);
    }
  }
  closedir(d);
  return true;
}
void ensureNodeWatch() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::set<std::string> first;
    if (!sndNodes(first)) {
      return;  // no /dev/snd: the timer's rescans alone
    }
    nodeWatch().ok = true;
    try {
      std::thread([seen = std::move(first)]() mutable { gl::threads::setName("gl-midi-watch");
        std::set<std::string> now;
        const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        int wd = -1;
        // reads every waiting event; the watch ends when the folder goes (watched again once it is back)
        const auto drain = [&] {
          alignas(inotify_event) char buf[4096];
          for (ssize_t got; (got = ::read(fd, buf, sizeof buf)) > 0;) {
            for (ssize_t at = 0; at + static_cast<ssize_t>(sizeof(inotify_event)) <= got;) {
              const auto* ev = reinterpret_cast<const inotify_event*>(buf + at);
              if (ev->mask & IN_IGNORED) wd = -1;
              at += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            }
          }
        };
        for (;;) {
          if (fd >= 0 && wd < 0) wd = inotify_add_watch(fd, "/dev/snd", IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB);
          if (wd >= 0) {
            pollfd p{};
            p.fd = fd;
            p.events = POLLIN;
            if (::poll(&p, 1, 60000) > 0) {
              drain();
              // a device's nodes come one after another: look once they are all there
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
              drain();
            }
          } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
          }
          gl::threads::woke();  // K-142: CPU report
          if (sndNodes(now) && now != seen) {
            seen.swap(now);
            nodeWatch().changes.fetch_add(1, std::memory_order_relaxed);
            uiwake::poke();  // K-142 (0.7.8): a hidden Gridline looks at once
          }
        }
      }).detach();
    } catch (...) {
      nodeWatch().ok = false;
    }
  });
}
#endif

}  // namespace

namespace {
std::atomic<uint64_t> gFakeChanges{0};  // stand-in devices plugged in or out (counted like the system's)
}

uint64_t midiSetupChanges() {
  const uint64_t fake = gFakeChanges.load(std::memory_order_relaxed);
#if defined(__MACOSX_CORE__) && !defined(GRIDLINE_MIDI_SNDIO)
  return fake + coreWatch().changes.load(std::memory_order_relaxed);
#elif defined(__linux__) && defined(__LINUX_ALSA__)
  return fake + nodeWatch().changes.load(std::memory_order_relaxed);
#else
  return fake;
#endif
}

bool midiSetupWatched() {
  static const bool fake = midiFakeMode();
  if (fake) return true;  // stand-in devices say when they come and go (midiFakeSetPort)
#if defined(__MACOSX_CORE__) && !defined(GRIDLINE_MIDI_SNDIO)
  CoreWatch& w = coreWatch();
  std::lock_guard<std::mutex> lk(w.mu);
  return w.done && w.ok;
#elif defined(__linux__) && defined(__LINUX_ALSA__)
  return nodeWatch().ok.load();
#else
  return false;
#endif
}

bool midiHotplugSupported() {
#if defined(GRIDLINE_MIDI_SNDIO)
  return false;  // listing sndio devices opens them: a rescan would see the ones Gridline holds open as gone
#else
  return true;
#endif
}

bool midiFakeMode() { return testMode() == "fake"; }

void midiFakeSetPort(bool output, const std::string& name, bool present) {
  FakePorts& f = fakePorts();
  {
    std::lock_guard<std::mutex> lk(f.mu);
    auto& v = output ? f.outs : f.ins;
    auto it = std::find(v.begin(), v.end(), name);
    if (present && it == v.end()) {
      v.push_back(name);
    } else if (!present && it != v.end()) {
      v.erase(it);
    } else {
      return;
    }
  }
  gFakeChanges.fetch_add(1, std::memory_order_relaxed);  // as the system says a device came or went
  uiwake::poke();
}

bool midiFakePresent(bool output, const std::string& name) {
  FakePorts& f = fakePorts();
  std::lock_guard<std::mutex> lk(f.mu);
  const auto& v = output ? f.outs : f.ins;
  return std::find(v.begin(), v.end(), name) != v.end();
}

MidiProbeResult probeMidiService() {
  MidiProbeResult r;
  const std::string mode = testMode();
  if (mode == "fail") {
    r.error = "error creating the MIDI client (simulated for testing)";
    return r;
  }
  if (mode == "hang") {  // a service that never answers
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(60));
    }
  }
  if (mode == "fake") {  // MIDIHP-1119: stand-in devices, the system is never asked
    FakePorts& f = fakePorts();
    std::lock_guard<std::mutex> lk(f.mu);
    r.inputs = f.ins;
    r.outputs = f.outs;
    r.ok = true;
    return r;
  }
  try {
#if defined(__MACOSX_CORE__) && !defined(GRIDLINE_MIDI_SNDIO)
    ensureCoreWatch();
#elif defined(__linux__) && defined(__LINUX_ALSA__)
    ensureNodeWatch();
#endif
    auto in = makeMidiIn();
    for (unsigned i = 0, n = in->getPortCount(); i < n; ++i) r.inputs.push_back(in->getPortName(i));
    auto out = makeMidiOut();
    for (unsigned i = 0, n = out->getPortCount(); i < n; ++i) r.outputs.push_back(out->getPortName(i));
    r.ok = true;
  } catch (const RtMidiError& e) {
    r = MidiProbeResult{};
    r.error = e.getMessage();
  } catch (const std::exception& e) {
    r = MidiProbeResult{};
    r.error = e.what();
  } catch (...) {
    r = MidiProbeResult{};
    r.error = "unknown error";
  }
  return r;
}

}  // namespace gl

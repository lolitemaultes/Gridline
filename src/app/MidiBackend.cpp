// Gridline — MIDI back ends (see MidiBackend.h). On the BSDs, RtMidi is built with its dummy
// API and the sndio classes below take its place behind RtMidiIn / RtMidiOut.
#include "MidiBackend.h"
#include "../../third_party/rtmidi/RtMidi.h"

#if defined(GRIDLINE_MIDI_SNDIO)
#include <poll.h>
#include <sndio.h>
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
    thread_ = std::thread([this] { run(); });
  }
  void openVirtualPort(const std::string&) override { error(RtMidiError::WARNING, "sndio: virtual ports are not supported"); }
  void closePort() override {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
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
    std::vector<pollfd> fds(static_cast<size_t>(mio_nfds(hdl_) > 0 ? mio_nfds(hdl_) : 1));
    while (!quit_) {
      const int n = mio_pollfd(hdl_, fds.data(), POLLIN);
      if (::poll(fds.data(), static_cast<nfds_t>(n), 50) < 0) continue;
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

}  // namespace gl

// Gridline - "Link to controller": which hardware control the user just moved.
//
// MidiControlDecoder turns raw MIDI into control events: plain messages one each, the NRPN / RPN
// parameter select and data entry CCs into one NRPN / RPN event, and a CC 0..31 followed by its
// CC 32..63 into one 14-bit value. Controller links on NRPN, RPN and 14-bit CCs match these events.
//
// LinkCapture watches the events while the link dialog listens and keeps the control that was moved
// on purpose, the latest one winning. It ignores what is not a move: messages that were waiting when
// the dialog opened, a control that only repeats its value or jitters by one step, a resting pitch
// wheel, and the keys (notes and key pressure) unless the user asked for a note or button. Pure logic
// (no device I/O, the caller passes the time), shared by the app and the tests.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include "Remote.h"

namespace gl {

// One control's message after decoding.
struct ControlEvent {
  RemoteSource source = RemoteSource::CC;
  int number = 0;        // CC, note, program, key pressure note, or NRPN / RPN parameter (0..16383)
  int channel = 0;       // 0..15
  int port = -1;         // input port number (-1 unknown)
  int value = 0;         // 0..127, or 0..16383 when `hiRes` (pitch bend, 14-bit CC, NRPN with fine data)
  bool hiRes = false;
  int delta = 0;         // NRPN / RPN increment (+1) or decrement (-1); `value` is not used then
  bool noteOff = false;  // a note-off (or note-on with velocity 0)
  double normalized() const { return hiRes ? value / 16383.0 : value / 127.0; }
};

class MidiControlDecoder {
 public:
  // Appends the events `m` completes (none for the NRPN / RPN select CCs 99, 98, 101, 100).
  void feed(const MidiMessage& m, std::vector<ControlEvent>& out);
  void reset() { ch_.clear(); }

 private:
  struct Chan {
    int paramMsb = -1, paramLsb = -1;  // the selected NRPN / RPN parameter (-1 none)
    bool rpn = false;
    bool fine = false;  // CC 38 seen for the selected parameter: its data has 14 bits
    int dataMsb = 0;
    int lastCc = -1;    // the previous message on this port and channel was this CC (-1: not a CC)
    std::array<int16_t, 32> msb{};    // the last value of each CC 0..31
    std::array<bool, 32> paired{};    // CC n is a 14-bit pair (its LSB came right after it)
  };
  std::map<int, Chan> ch_;  // by port * 16 + channel
};

// True when a link on an NRPN, an RPN or a 14-bit CC (decoded links) takes `e`.
bool linkUsesDecoder(const ControllerLink& l);
bool remoteMatchesEvent(const ControllerLink& l, const ControlEvent& e);
// The link's input (0..1) of a matching event.
double remoteEventInput(const ControllerLink& l, const ControlEvent& e);

class LinkCapture {
 public:
  struct Options {
    bool notes = false;      // notes, pads and key pressure count (a button or note link)
    double settle = 0.15;    // seconds after start that only learn where each control rests
    double lonePress = 0.12; // a single CC with nothing after it is a press after this long
  };
  struct Captured {
    RemoteSource source = RemoteSource::CC;
    int number = 0, channel = 0, port = -1;
    bool hiRes = false;
    std::string device;      // the input's name
    bool surface = false;    // the port belongs to a control surface (the link takes this control over from it)
    double value = 0;        // latest position 0..1 (the meter)
    double valueTime = 0;    // when it last moved
    int captures = 0;        // how many times a control was (re)captured since start
  };
  void start(double now, Options o);
  void stop() { listening_ = false; }
  void reset() { start(0, o_), listening_ = false; }  // forgets what was captured, not listening
  void setNotes(bool on) { o_.notes = on; }  // keys and pads count from now on (or no longer)
  bool listening() const { return listening_; }
  bool hasCapture() const { return captured_.captures > 0; }
  const Captured& captured() const { return captured_; }
  // An event from input `device`. True when it (re)captured a control (the caller copies it into the link).
  bool feed(const ControlEvent& e, const std::string& device, bool surfacePort, double now);
  // A lone press that waited long enough is captured now (call every frame). True when it captured.
  bool tick(double now);
  // Writes the captured control into a link's source (type, number, channel, port, 14 bits).
  void apply(ControllerLink& l) const;
  // Why the last event was not taken (tests and the log): "settling", "keys", "repeat", ...
  const char* lastIgnored() const { return ignored_; }

 private:
  struct Ctl {
    int anchor = -1;     // the value it rests at / was captured at (-1: never seen)
    int last = -1;
    int repeats = 0;     // the same value again, in a row
    double lastTime = 0, lastGap = 0;
    bool stream = false; // sends the same value over and over on its own
  };
  using Key = std::tuple<std::string, int, int, int>;  // device, channel, source, number
  static Key keyOf(const std::string& device, const ControlEvent& e);
  void take(const ControlEvent& e, const std::string& device, bool surfacePort, double now);
  Options o_;
  bool listening_ = false;
  double start_ = 0;
  std::map<Key, Ctl> ctl_;
  Captured captured_;
  Key capturedKey_;
  bool pending_ = false;  // a lone CC press waiting for lonePress
  ControlEvent pendingEv_;
  std::string pendingDevice_;
  bool pendingSurface_ = false;
  double pendingTime_ = 0;
  const char* ignored_ = "";
  bool lone_ = false;     // the capture is a lone press (undone if the control turns out to stream)
  Captured prev_;         // what was captured before it
  Key prevKey_;
};

}  // namespace gl

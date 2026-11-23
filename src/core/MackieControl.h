// Gridline — K-214: control surfaces that speak the Mackie Control (MCU) or HUI protocol
// (Mackie Control Universal / Pro, X-Touch and X-Touch One in their MC mode, extenders,
// and the many surfaces that copy them). Banks of 8 mixer inserts on motor faders, pan on
// the encoders with their LED rings, rec arm / solo / mute / select per strip, bank and
// channel shift, transport and the jog wheel, the scribble strips (names, levels), the
// timecode display (bars, beats, ticks) and the strip meters.
//
// A Surface is one controller: the main unit plus up to three extenders (unit 1..3, eight
// strips each, to the right of the main unit). Messages come in with their unit; replies go
// out through `out(unit, bytes)`. Every edit goes through Document (undoable, collaborative),
// selection / arm / transport through the ControllerHost. The protocols are the public ones
// (note / CC / pitch bend numbers below); no vendor code or data.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "ControllerScript.h"
#include "Remote.h"

namespace gl::mackie {

enum class Protocol { Mcu, Hui };

struct Options {
  Protocol protocol = Protocol::Mcu;
  int extenders = 0;        // 0..3 (MCU), eight strips each
  bool singleStrip = false; // X-Touch One style: one strip that follows the selected insert
};

// MCU note numbers (buttons in, LEDs out; velocity 127 = pressed / lit).
enum McuNote : uint8_t {
  NRecArm = 0x00, NSolo = 0x08, NMute = 0x10, NSelect = 0x18, NVPotPush = 0x20,
  NBankLeft = 0x2E, NBankRight = 0x2F, NChannelLeft = 0x30, NChannelRight = 0x31,
  NCycle = 0x56, NClick = 0x59, NUndo = 0x51, NRewind = 0x5B, NForward = 0x5C, NStop = 0x5D,
  NPlay = 0x5E, NRecord = 0x5F, NFaderTouch = 0x68, NMasterTouch = 0x70
};
// MCU CCs: encoders in (relative), rings out, jog wheel, timecode digits.
enum McuCc : uint8_t { CVPot = 0x10, CRing = 0x30, CJog = 0x3C, CTimecode = 0x40 };

class Surface {
 public:
  using Out = std::function<void(int unit, const std::vector<uint8_t>& bytes)>;
  Surface(script::ControllerHost& host, Options o, Out out);

  // One message from unit `unit` (0 = main). True when the surface used it.
  bool onMidi(int unit, const MidiMessage& m);
  // About 30 times a second: sends what changed (faders not held, LEDs, rings, displays,
  // meters). `levels`: peak level 0..1 per mixer index (master 0), may be empty.
  void idle(double now, const std::vector<float>& levels = {});
  // Sends everything again (a surface was (re)connected).
  void refresh();

  int strips() const { return opt_.singleStrip ? 1 : 8 * (1 + opt_.extenders); }
  int units() const { return opt_.singleStrip ? 1 : 1 + opt_.extenders; }
  int bank() const { return bank_; }  // strip 0's place among the inserts the mixer shows (GitHub #58: hidden ones skipped)
  // Mixer index (into Song::mixer) on strip s, -1 = none.
  int mixerOf(int strip) const;
  const Options& options() const { return opt_; }

  // Text helpers (exposed for the tests): a strip's 7-character MCU name / 4-character HUI name.
  static std::string stripName(const std::string& name, size_t width);

 private:
  struct Shown {  // what the surface shows now (-1 / empty: unknown, sent on the next idle)
    int fader = -1, ring = -1;
    int leds = -1;  // bits: arm, solo, mute, select
    std::string top, bottom;
  };
  bool onMcu(int unit, const MidiMessage& m);
  bool onHui(const MidiMessage& m);
  void button(int strip, int what);  // 0 arm, 1 solo, 2 mute, 3 select, 4 v-pot push
  void transport(int key, bool down);
  void moveBank(int by);
  void setFader(int strip, double norm);
  void turn(int strip, int steps);
  void send(int unit, std::initializer_list<uint8_t> b) { out_(unit, std::vector<uint8_t>(b)); }
  void sendText(int unit, int offset, const std::string& text);  // MCU scribble strip
  void sendTimecode();
  double faderNorm(int mixer) const;
  double panNorm(int mixer) const;
  std::string levelText(int mixer) const;

  script::ControllerHost& host_;
  Options opt_;
  Out out_;
  int bank_ = 0;
  std::vector<Shown> shown_;
  std::vector<bool> touched_;
  bool masterTouched_ = false;
  int masterFader_ = -1;
  int transportLeds_ = -1;
  std::array<char, 10> timecode_{};
  bool timecodeSent_ = false;
  int huiZone_ = -1;  // HUI: the zone the next port message belongs to
  std::array<int, 8> huiFaderMsb_{};
  double lastPing_ = -10;
};

}  // namespace gl::mackie

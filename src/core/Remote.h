// Gridline — MIDI remote control (J-17): how a hardware control's messages
// drive a linked parameter or action. Mapping curves and formulas, pickup /
// scaling takeover, relative encoders, note and button modes. Pure logic (no
// device I/O), shared by the app and the tests.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Expr.h"
#include "Song.h"

namespace gl {

// One short MIDI message from an input port.
struct MidiMessage {
  uint8_t status = 0, d1 = 0, d2 = 0;
  int port = -1;  // input port number of the device (-1 = unknown)
  int kind() const { return status & 0xF0; }
  int channel() const { return status & 0x0F; }
  bool noteOn() const { return kind() == 0x90 && d2 > 0; }
  bool noteOff() const { return kind() == 0x80 || (kind() == 0x90 && d2 == 0); }
};

// True when `m` comes from the control the link listens to.
bool remoteMatches(const ControllerLink& l, const MidiMessage& m);
// Absolute input 0..1 carried by a matching message (CC value, velocity,
// 14-bit pitch bend, pressure); note-offs give 0.
double remoteInput(const ControllerLink& l, const MidiMessage& m);
// "Pressed" state of a button-like message (note on / off, CC >= 64).
bool remoteButtonDown(const ControllerLink& l, const MidiMessage& m);
// Signed detent count of a relative encoder value.
int decodeRelative(RemoteEncoder e, int value);
// Encodes a detent count (tests, controller output feedback).
int encodeRelative(RemoteEncoder e, int delta);

// Link space: input u (0..1) → invert → curve / formula → w (0..1).
// `formula` is the compiled ControllerLink::formula (may be empty).
double remoteCurve(const ControllerLink& l, double u, const Expr* formula = nullptr);
// w → normalized target value in [min, max].
inline double remoteRange(const ControllerLink& l, double w) { return l.min + (l.max - l.min) * w; }
// Compiles a link formula (variable `input`); false + message on error.
bool compileRemoteFormula(const std::string& text, Expr& out, std::string* err = nullptr);

// Mapping presets offered by the remote control dialog (curve, formula,
// encoder and sensitivity in one pick).
struct RemotePreset {
  const char* group;  // "Absolute", "Relative", "Buttons"
  const char* name;
  ModCurve curve;
  const char* formula;  // nullptr = keep / none
  RemoteEncoder encoder;
  float sensitivity;
  RemoteButton button;
};
const std::vector<RemotePreset>& remotePresets();
void applyRemotePreset(ControllerLink& l, const RemotePreset& p);

const char* remoteSourceName(RemoteSource s);
const char* remoteTakeoverName(RemoteTakeover t);
const char* remoteEncoderName(RemoteEncoder e);
const char* remoteButtonName(RemoteButton b);
const char* remoteActionName(RemoteAction a);
// "CC 74 · ch 1", "Note C5 · any ch", "Pitch bend", …
std::string remoteSourceLabel(const ControllerLink& l);
// FL-style note name (60 = C5).
std::string midiNoteName(int note);

// Runs links: keeps per-link takeover state (last hardware position,
// picked-up flag, toggle state) and compiled formulas.
class RemoteMapper {
 public:
  // Applies a matching message to a parameter link. `cur` is the target's
  // current normalized value; `step` the normalized size of one step for
  // Increment buttons (0 = 1/8). Returns true and sets *out when the target
  // should change.
  bool process(const ControllerLink& l, const MidiMessage& m, double cur, double* out, double step = 0.0);
  // Action links: true on the press of the control (rising edge).
  bool trigger(const ControllerLink& l, const MidiMessage& m);
  // Pickup mode: true while the link waits for the control to reach the value.
  bool waiting(const ControllerLink& l) const;
  void forget(const ControllerLink& l);
  void reset() { st_.clear(); }

  static uint64_t key(const ControllerLink& l);
  static constexpr double kPickupTolerance = 0.02;

 private:
  struct State {
    double lastIn = -1;   // previous mapped hardware position (normalized target space), -1 = unknown
    double lastOut = -1;  // value this link last wrote, -1 = none
    bool picked = false;
    bool toggled = false;
    bool down = false;
  };
  const Expr* formula(const std::string& text);
  std::map<uint64_t, State> st_;
  std::map<std::string, Expr> exprs_;
};

// --- input routing & note handling ------------------------------------------

enum class VelocityCurve : uint8_t { Linear = 0, Soft, Hard, Fixed, Count };
const char* velocityCurveName(VelocityCurve c);
// Applies a velocity curve to a note-on velocity (1..127 stays 1..127).
int applyVelocityCurve(VelocityCurve c, int velocity, int fixed = 100);

}  // namespace gl

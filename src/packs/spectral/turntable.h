// Gridline — "spectral" pack: Turntable (gl.turntable), an instrument that
// plays one loaded sample on a platter you scratch with the mouse, automation
// or a controller. The whole sample is one revolution of the platter: the
// platter angle is the play position and it loops.
//
// Two ways the platter moves:
//  * motor: the platter is pulled toward the motor speed (Reverse / Pause /
//    Forward times Speed) with the Acceleration's inertia;
//  * hand (Hold on, or the platter / waveform grabbed in the editor): the
//    platter follows Position (+ whole Turns) through a damped spring shaped
//    by the four Smooth controls.
// Notes (Gridline's instrument side): while a key is held the motor runs
// forward (at the key's pitch with Key track on); "Cue + motor" also starts
// each note from the Cue point at full speed.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Generators.h"

namespace gl::packs::spectral {

enum TurntableParam {
  TtPosition,    // 0..1 (shown in %): hand position on the sample (one revolution)
  TtMute,        // toggle: output cut (the crossfader)
  TtSpeed,       // 0..2 (shown in %): motor speed (1 = the sample's own speed)
  TtAccel,       // 0..1: how fast the platter follows the motor
  TtPan,         // -1..1
  TtVolume,      // 0..1: linear output level
  TtHold,        // toggle: the hand holds the platter (follows Position)
  TtSense,       // 0..1: mouse travel per pixel in the editor
  TtTransport,   // choice: Reverse / Pause / Forward (the motor)
  TtSmooth1,     // 0..1 hand spring: settle time
  TtSmooth2,     // 0..1 hand spring: momentum
  TtSmooth3,     // 0..1 hand spring: stiffness
  TtSmooth4,     // 0..1 hand spring: carry (momentum share of each step)
  TtTurns,       // -9..9 whole revolutions added to Position (editor drags past the ends)
  // ---- Gridline (instrument side)
  TtNotes,       // choice: Off / Motor / Cue + motor
  TtKeyTrack,    // toggle: the note's pitch sets the motor speed (C5 = Speed)
  TtCue,         // 0..1: where "Cue + motor" notes start
  TtCount
};

enum TurntableNotes { TnOff = 0, TnMotor = 1, TnCue = 2 };

// Live values the editor reads (audio thread writes, relaxed).
struct TurntableTelemetry {
  std::atomic<float> pos{0};      // play position 0..1 (wrapped)
  std::atomic<float> speed{0};    // platter speed, 1 = the sample's own speed forward
  std::atomic<float> gain{0};     // output gain after the mute / volume smoothing
  std::atomic<bool> held{false};  // the hand is on the platter
  std::atomic<bool> loaded{false};
};

// The runtime's telemetry (null for other generators); used by the editor.
const TurntableTelemetry* turntableTelemetry(const Generator* g);

// Catalogue entry, runtime and prepared data (turntable_info / turntable_dsp).
PluginInfo turntableInfo();
std::unique_ptr<Generator> createTurntable();
bool prepareTurntableAux(const PluginState& state, ChannelAux& aux);

// Mapping from the reference plugin's 14 stored values (FL import, tests):
// internal values as the reference stores them (see FlpFx2Pitch.cpp).
void turntableFromReference(const float* v, int n, std::vector<float>& params);

}  // namespace gl::packs::spectral

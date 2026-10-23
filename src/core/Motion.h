// Gridline - J-91: interface motion switches and the metronome swing.
//
// Settings > Interface > Motion "Interface animations" (ui.animations, on by
// default) is the master switch of decorative motion: off (or while the
// system asks for reduced motion) the metronome icon stays still, key and pad
// lights switch instead of fading, snapped drags and wheel steps jump instead
// of gliding and scroll edges stay hard. That also saves the frames those
// animations would draw. Pure functions (unit tested in test_j91).
#pragma once

namespace gl::motion {

struct Switches {
  bool animations = true;   // the master switch, after reduced motion
  bool smoothSnap = true;   // J-37 / J-67 gliding drags
  bool smoothScroll = true; // J-67
  bool elastic = true;      // J-67
};
// What is on, from the settings and the system's reduced-motion preference.
Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool reducedMotion);

// The metronome icon's horizontal scale (-1 .. 1) at song position `beats`
// (quarter notes) and tempo `bpm`: it faces one way on even beats and the
// other on odd beats, swinging over on each beat in a short eased turn
// (kSwingSeconds, at most 45 % of a beat) like a pendulum ticking in tempo.
// 1 when `swinging` is false (metronome off, transport stopped, animations off).
constexpr double kSwingSeconds = 0.14;
double metronomeSwing(double beats, double bpm, bool swinging);

}  // namespace gl::motion

// Gridline - J-91: interface motion switches; J-97: the metronome tick.
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
  bool smoothZoom = true;   // J-188 gliding zoom
};
// What is on, from the settings and the system's reduced-motion preference.
Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool smoothZoom, bool reducedMotion);
// The same with smooth zooming on (as before J-188).
Switches effective(bool animations, bool smoothSnap, bool smoothScroll, bool elastic, bool reducedMotion);

// The metronome icon's horizontal scale (+1 faces right, -1 mirrored) at song
// position `beats` (quarter notes or the metronome's beat): a hard tick-tock
// that snaps over exactly on each beat boundary, with no in-between frames.
// `anchorBeat` is the beat the ticking started in (metronome switched on or
// transport started): that beat shows the rest pose (+1), the next one is
// mirrored, and so on. +1 when `ticking` is false (metronome off, transport
// stopped, counting in, animations off / reduced motion) or the position is
// invalid, so toggling the metronome never animates: it just shows the rest pose.
double metronomeTick(double beats, bool ticking, double anchorBeat);

// Remembers where the ticking started so a toggle (or play) mid-beat holds the
// rest pose until the next beat boundary instead of jumping to a mirrored face.
class MetronomeTicker {
 public:
  double update(double beats, bool ticking);

 private:
  bool ticking_ = false;
  double anchor_ = 0.0;
};

}  // namespace gl::motion

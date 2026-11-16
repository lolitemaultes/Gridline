// Gridline - K-261: Tracer (gl.delay) has one view, the reference panel, whatever engine a
// saved state plays.
//
// A Tracer keeps the engine it was saved with (DlyModel: 0 the first Gridline delay, 1 the
// current one, 2 the classic tempo delay FL's Delay and Delay 2 import as, 3 the stereo delay
// reference), so every old preset, project and import sounds exactly as before and its saved
// state is never rewritten. There is no model chooser any more. The panel's controls bind to
// whatever parameter the state's engine reads for that job (an older engine's feedback, its
// time, its stereo mode ...); a control the engine has no counterpart for is shown dimmed.
// New instances use the current engine. Pure (tests/test_k261.cpp).
#pragma once
#include "PluginParams.h"

namespace gl::tracer {

// The parameter that the panel's control for `refParam` (a parameter of the current engine,
// pp::Dly*) edits on engine `engine`, or -1 when that engine has no such control. Engine 3 has
// its own normalised controls on the same panel (PluginEditorsDelayRef.cpp) and is not mapped here.
inline int control(int engine, int refParam) {
  using namespace pp;
  if (engine == 1 || engine == 3) return refParam;
  if (engine == 0) {  // the first Gridline delay: most controls are shared
    switch (refParam) {
      case DlyTicks: return DlyTime;          // its synced time is a note-value choice
      case DlyTimeR: return DlyOffset;        // R/L offset in ms
      case DlyType3: return DlyMode;          // normal, ping-pong, invert, mono, off
      case DlySpread3: return DlyStereoSpread;
      case DlyCutoff3: return DlyCutoff;
      case DlyFilter3: return DlyFilterType;  // one-pole pair, LP, HP, BP, off
      case DlyRate3: return DlyRateReduce;
      case DlyModRate3: return DlyModRate;
      default: return refParam;
    }
  }
  if (engine == 2) {  // the classic tempo delay
    switch (refParam) {
      case DlyInput: return DlyClsIn;
      case DlyTicks: return DlyTicks;
      case DlyTimeR: return DlyClsOffset;
      case DlyType3: return DlyClsMode;  // normal, swap, ping-pong
      case DlyFeedback: return DlyClsFb;
      case DlyCutoff3: return DlyClsCoef;
      case DlyDry: return DlyClsDry;
      case DlyTone: return DlyClsPan;  // the column's centred wheel: the classic input pan
      default: return -1;
    }
  }
  return -1;
}

// The engine a state plays (its saved DlyModel; the current engine when out of range).
inline int engineOf(double model) {
  const int m = static_cast<int>(model + 0.5);
  return m >= 0 && m <= 3 ? m : 1;
}

}  // namespace gl::tracer

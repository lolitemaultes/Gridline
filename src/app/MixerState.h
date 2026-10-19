// Gridline — mixer window view state (J-12): strip layout, rows, extended
// slot view, meter options, multi-track selection. (The record arm is MixerTrack::armed, J-8.)
#pragma once
#include <set>
#include "../core/Types.h"

namespace gl {

struct MixerViewState {
  enum Layout : int { Compact = 0, Wide = 1, ExtraLarge = 2 };
  enum MeterMode : int { Peak = 0, Rms = 1, PeakRms = 2 };
  int layout = Wide;          // mirrors the "ui.mixerLayout" setting (View > Mixer layout)
  bool layoutOverride = false;  // dev screenshots: keep `layout` instead of the setting
  int rows = 1;              // inserts wrap into this many rows
  bool extended = false;     // strips list their effect slots (names + mix knobs)
  bool peakHold = true;      // meters hold their peaks for a moment
  int meterMode = Peak;
  bool masterScale = true;   // dB scale beside the master meter
  bool waveforms = false;    // post-fader waveform on every strip
  bool showRouting = false;  // routing overview window
  std::set<ObjectId> sel;    // selected tracks (besides App::selMixer, the focused one)
};

}  // namespace gl

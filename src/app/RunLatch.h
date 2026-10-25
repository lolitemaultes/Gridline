// Gridline - J-110: the state of an editor's own Play / preview / run button
// (Seltmann's pattern, Kerf's slice play, the chord editor's progression
// preview), kept in step with what actually sounds.
//
// An editor never trusts its own toggle alone. Each frame it calls sync()
// with what the engine / its DSP reports right now, and the button shows
// `on`. The latch turns off by itself when
//  - the engine's stop epoch moved since start() (AudioEngine::stopEpoch():
//    the transport Stop, Stop twice (panic), Space, Tools > Stop all sound, a
//    MIDI / remote / host Stop, a focus loss): the engine has already
//    released or killed the sound, so one click starts it again;
//  - the DSP was seen running and is not any more (a one-shot came to its
//    end, the instrument was replaced or killed elsewhere);
//  - the DSP never reported running within `graceFrames` frames (no audio
//    device, no instrument: nothing plays, so the button must not say so).
// UI thread only; the inputs are the engine's atomics.
#pragma once
#include <cstdint>
#include "../engine/AudioEngine.h"

namespace gl {

struct RunLatch {
  enum class End { None, Stopped, Finished };
  bool on = false;
  uint32_t epoch = 0;
  bool seen = false;
  int waited = 0;

  void start(const AudioEngine& e) {
    on = true;
    epoch = e.stopEpoch();
    seen = false;
    waited = 0;
  }
  void stop() { on = false; }

  // `running`: what the DSP reports now (true when the editor has no DSP
  // state to read: only the stop epoch counts then). Returns why the latch
  // just turned off (None: it did not): Stopped = by the engine (the sound is
  // already released), Finished = it ended by itself (the caller releases the
  // key it still holds).
  End sync(const AudioEngine& e, bool running, int graceFrames = 90) {
    if (!on) return End::None;
    if (e.stopEpoch() != epoch) {
      on = false;
      return End::Stopped;
    }
    if (running) {
      seen = true;
      return End::None;
    }
    if (seen || ++waited > graceFrames) {
      on = false;
      return End::Finished;
    }
    return End::None;
  }
};

}  // namespace gl

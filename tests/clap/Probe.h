// What the in-tree CLAP test plugins saw last (exported as gridline_clap_probe()).
#pragma once
#include <clap/clap.h>

struct GridlineClapProbe {
  int transports = 0;
  clap_event_transport_t transport{};
  bool hadTransport = false;
  int renderMode = 0;      // last clap.render mode set
  double lastMod = 0;      // last PARAM_MOD amount on the synth's Gain
  int lastParamTime = -1;  // time of the last Level event (gain effect)
  int latencyRestarts = 0;
  uint32_t lastFrames = 0;
  int views = 0;  // interfaces attached now (macOS)
  // J-109: note-offs and channel-mode controllers the synth received.
  int noteOffs = 0;
  int allNotesOff = 0;  // CC 123 (any channel)
  int allSoundOff = 0;  // CC 120 (any channel)
  int sustainOff = 0;   // CC 64 = 0
};

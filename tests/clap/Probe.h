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
};

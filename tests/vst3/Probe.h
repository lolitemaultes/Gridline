// What the in-tree VST3 test processors saw last (exported as gridline_vst3_probe()).
#pragma once
#include "pluginterfaces/vst/ivstprocesscontext.h"

struct GridlineVst3Probe {
  Steinberg::Vst::ProcessContext context{};
  bool hadContext = false;
  Steinberg::int32 processMode = -1;
  Steinberg::int32 blocks = 0;
  int views = 0;  // interfaces attached now (macOS)
};

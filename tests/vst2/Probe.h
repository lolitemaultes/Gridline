// What the in-tree VST2 test plugins saw last (exported as gridline_vst2_probe()).
#pragma once
#include "host/vst2/Vst2Abi.h"

struct GridlineVst2Probe {
  gl::vst2abi::TimeInfo time{};
  int timeCalls = 0;
  int processLevel = 0;
  int bypassCalls = 0;
};

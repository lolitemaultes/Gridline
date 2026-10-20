// What the in-tree LV2 test plugins saw last (exported as
// gridline_lv2_probe() from the DSP binary, gridline_lv2_ui_probe() from
// the interface binary).
#pragma once
#include <cstdint>

struct GridlineLv2Probe {
  // synth: time:Position objects received and the last one's values
  int positions = 0;
  int64_t frame = -1;
  float speed = -1, bpm = 0, beatsPerBar = 0, barBeat = -1;
  int beatUnit = 0;
  int64_t bar = -1;
  double beat = -1;
  int notesOn = 0;
  // state plugin
  int maxBlock = 0;        // options: buf-size maxBlockLength seen at instantiate
  int works = 0;           // worker work() calls
  int responses = 0;       // work_response() calls
  bool workOnRunThread = false;  // the last work() ran inside run() (synchronous)
  float freewheel = -1;    // freeWheeling port value seen in run()
  int restores = 0;
  int runs = 0;
};

struct GridlineLv2UiProbe {
  int instances = 0;     // Cocoa interface instances alive
  int portEvents = 0;
  float lastGain = -1;   // last value of the gain port the interface received
  int width = 0, height = 0;
  int idles = 0;
  bool resizeAsked = false;  // ui:resize host feature answered
  // external interface (state plugin)
  int shows = 0, hides = 0, runs = 0;
  bool closeNow = false;  // set by the test: the next run() reports the window closed
  int atomEvents = 0;     // atom messages received through port_event
  int lastAtomInt = 0;    // value of the last Int atom received
};

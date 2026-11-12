// What the in-tree CLAP test plugins saw last (exported as gridline_clap_probe()).
#pragma once
#include <clap/clap.h>
#include "../NoteLog.h"

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
  GridlineNoteLog notes;  // what the synth received
  // K-144: MPE: channels notes started on (bit per channel), the zone message (RPN 6 on
  // channel 0, -1 = none) and the bend range RPN 0 seen on channel 1 (-1 = none).
  uint32_t noteChannels = 0;
  int mpeZone = -1, mpeRange = -1;
  int rpnMsb[16] = {};
  // K-193: meter changes seen: each process call whose time signature differs from the one
  // before records its numerator and the transport position (beats) at its first sample.
  int tsigChanges = 0;
  int tsigNum[8] = {};
  double tsigBeats[8] = {};
  int lastTsig = 0;
  bool throwInProcess = false;  // K-9: process throws, as a bridge does when its other process is gone
};

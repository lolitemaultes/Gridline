// What the strict test instruments (tests/vst3/StrictPlugin.cpp, tests/clap/StrictPlugin.cpp)
// saw from the host: every call the host made out of order or with buffers that do not match
// the negotiated layout sets a bit in `violations` (and the plugin outputs silence, as picky
// plugins do). Exported as gridline_strict_probe().
#pragma once
#include <cstdint>

enum GridlineStrictViolation : uint32_t {
  kStrictArrangeWhileActive = 1u << 0,    // bus layout changed while active
  kStrictBusWhileActive = 1u << 1,        // bus switched on / off while active
  kStrictSetupWhileActive = 1u << 2,      // processing set up (rate, block) while active
  kStrictActiveWithoutSetup = 1u << 3,    // activated before the sample rate / block size were set
  kStrictProcessInactive = 1u << 4,       // process before activation (or after deactivation)
  kStrictProcessNotStarted = 1u << 5,     // process before processing was started
  kStrictBlockTooLong = 1u << 6,          // more frames than the maximum block given at setup
  kStrictLayout = 1u << 7,                // buffers do not match the active buses (count, channels, null)
  kStrictInputs = 1u << 8,                // audio inputs passed to a plugin that has none
  kStrictDeactivateProcessing = 1u << 9,  // deactivated while still processing
  kStrictEventsInactive = 1u << 10,       // notes sent to an event bus that is switched off
  kStrictWrongThread = 1u << 11,          // CLAP: start_processing / process off the audio thread
};

struct GridlineStrictProbe {
  uint32_t violations = 0;
  int64_t frames = 0;  // frames processed since processing started
  int32_t blocks = 0, zeroBlocks = 0, minBlock = 0, maxBlock = 0;
  int32_t setupMaxBlock = 0;
  double sampleRate = 0;
  int32_t activeOutputs = 0;  // output buses active at the last process call
  int32_t notesOn = 0;        // note-ons received while ready
  int32_t starts = 0;         // processing started (setProcessing / start_processing)
  int32_t mainBus = 0;        // where the class has its main output
  void reset() { *this = GridlineStrictProbe{}; }
};

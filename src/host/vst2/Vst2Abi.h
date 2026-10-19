// Gridline — clean-room description of the binary interface of "VST 2.x"
// style plugins, written for interoperability from publicly documented facts
// about the ABI (the layout of the effect structure, the numeric dispatcher
// opcodes, event and time-info records as described in public format notes and
// community reverse-engineering write-ups). It contains no code or text from
// Steinberg's VST 2 SDK; all identifiers are Gridline's own. Only what the
// host uses is declared.
//
// A plugin binary exports an entry function (`VSTPluginMain`, or `main_macho`
// / `main` in older binaries) that takes the host callback and returns a
// pointer to an Effect record.
#pragma once
#include <cstdint>

namespace gl::vst2abi {

struct Effect;

// Host callback and the plugin's function slots. `intptr_t` slots are pointer
// sized; the opcode, index and float arguments are 32-bit.
using HostCallback = intptr_t (*)(Effect* fx, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using DispatchFn = intptr_t (*)(Effect* fx, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessFn = void (*)(Effect* fx, float** inputs, float** outputs, int32_t frames);
using ProcessDoubleFn = void (*)(Effect* fx, double** inputs, double** outputs, int32_t frames);
using SetParamFn = void (*)(Effect* fx, int32_t index, float value);
using GetParamFn = float (*)(Effect* fx, int32_t index);
using EntryFn = Effect* (*)(HostCallback host);

constexpr int32_t kMagic = 0x56737450;  // the four bytes "VstP" as a big-endian integer

// The record a plugin returns from its entry function.
struct Effect {
  int32_t magic;
  DispatchFn dispatch;
  ProcessFn processAccumulating;  // legacy, unused by Gridline
  SetParamFn setParam;
  GetParamFn getParam;
  int32_t programCount;
  int32_t paramCount;
  int32_t inputCount;
  int32_t outputCount;
  int32_t flags;
  intptr_t reserved1;
  intptr_t reserved2;
  int32_t latency;  // "initial delay" in samples
  int32_t legacyQualities1;
  int32_t legacyQualities2;
  float legacyIoRatio;
  void* pluginObject;
  void* hostData;  // free for the host
  int32_t uniqueId;
  int32_t pluginVersion;
  ProcessFn processReplacing;
  ProcessDoubleFn processDoubleReplacing;
  char future[56];
};

// Effect::flags bits.
enum : int32_t {
  kFlagHasEditor = 1 << 0,
  kFlagCanReplace = 1 << 4,
  kFlagChunks = 1 << 5,  // state is an opaque chunk (get/set chunk opcodes)
  kFlagInstrument = 1 << 8,
  kFlagSilentWhenStopped = 1 << 9,
  kFlagCanDouble = 1 << 12,
};

// Host → plugin opcodes (dispatch).
enum : int32_t {
  opOpen = 0,
  opClose = 1,
  opSetProgram = 2,
  opGetProgram = 3,
  opGetProgramName = 5,
  opGetParamUnit = 6,     // ptr: char buffer
  opGetParamText = 7,     // ptr: char buffer, current value as text
  opGetParamName = 8,     // ptr: char buffer
  opSetSampleRate = 10,   // opt: sample rate
  opSetBlockSize = 11,    // value: max frames
  opPowerChanged = 12,    // value: 1 = resume, 0 = suspend
  opEditorRect = 13,      // ptr: Rect** out
  opEditorOpen = 14,      // ptr: native parent view
  opEditorClose = 15,
  opEditorIdle = 19,
  opGetChunk = 23,        // index: 0 bank, 1 program; ptr: void** out; returns size
  opSetChunk = 24,        // index: 0 bank, 1 program; value: size; ptr: data
  opProcessEvents = 25,   // ptr: EventList*
  opCanAutomate = 26,     // index: parameter
  opGetProgramNameIndexed = 29,  // index: program; ptr: char buffer
  opGetOutputInfo = 34,   // index: output pin; ptr: PinInfo*
  opGetCategory = 35,
  opSetBypass = 44,
  opGetName = 45,         // ptr: char buffer
  opGetVendor = 47,       // ptr: char buffer
  opGetProduct = 48,      // ptr: char buffer
  opGetVendorVersion = 49,
  opCanDo = 51,           // ptr: capability string; returns 1 yes, -1 no, 0 unknown
  opGetTailSize = 52,
  opGetParamInfo = 56,    // index: parameter; ptr: ParamProps*
  opGetApiVersion = 58,
  opBeginSetProgram = 67,
  opEndSetProgram = 68,
  opShellNext = 70,       // ptr: char[64] name out; returns the next sub-plugin id (0 = done)
  opStartProcess = 71,
  opStopProcess = 72,
  opSetPrecision = 77,    // value: 0 = 32-bit floats
};

// Plugin → host opcodes (host callback).
enum : int32_t {
  hostAutomate = 0,         // index: parameter; opt: new value (from the plugin's editor)
  hostVersion = 1,
  hostCurrentId = 2,        // the sub-plugin id being instantiated (shells)
  hostIdle = 3,
  hostWantsMidi = 6,
  hostGetTime = 7,          // returns TimeInfo*
  hostProcessEvents = 8,    // plugin MIDI output (ignored)
  hostIoChanged = 13,
  hostResizeWindow = 15,    // index: width, value: height
  hostGetSampleRate = 16,
  hostGetBlockSize = 17,
  hostGetInputLatency = 18,
  hostGetOutputLatency = 19,
  hostGetProcessLevel = 23,
  hostGetAutomationState = 24,
  hostGetVendor = 32,
  hostGetProduct = 33,
  hostGetVendorVersion = 34,
  hostCanDo = 37,
  hostGetLanguage = 38,
  hostGetDirectory = 41,
  hostUpdateDisplay = 42,
  hostBeginEdit = 43,       // index: parameter
  hostEndEdit = 44,         // index: parameter
};

// Plugin categories (opGetCategory).
enum : int32_t { catUnknown = 0, catEffect = 1, catInstrument = 2, catShell = 10, catGenerator = 11 };

// Editor rectangle (opEditorRect).
struct Rect {
  int16_t top, left, bottom, right;
};

// One MIDI event inside an EventList.
struct MidiEvent {
  int32_t type;      // 1 = MIDI
  int32_t byteSize;  // sizeof(MidiEvent)
  int32_t frameOffset;
  int32_t flags;
  int32_t noteLength;
  int32_t noteOffset;
  uint8_t data[4];   // status, data1, data2, 0
  int8_t detune;     // cents, -64..63
  uint8_t releaseVelocity;
  uint8_t reserved1, reserved2;
};
static_assert(sizeof(MidiEvent) == 32, "MIDI event record layout");

// Event list header followed by `count` pointers (a variable-length array).
template <int N>
struct EventList {
  int32_t count;
  intptr_t reserved;
  void* events[N];
};

// Transport (hostGetTime).
struct TimeInfo {
  double samplePos;
  double sampleRate;
  double systemNanos;
  double ppqPos;
  double tempo;
  double barStartPpq;
  double cycleStartPpq;
  double cycleEndPpq;
  int32_t sigNumerator;
  int32_t sigDenominator;
  int32_t smpteOffset;
  int32_t smpteRate;
  int32_t samplesToClock;
  int32_t flags;
};
enum : int32_t {
  timeChanged = 1,
  timePlaying = 2,
  timeCycleActive = 1 << 2,
  timeRecording = 1 << 3,
  timeNanosValid = 1 << 8,
  timePpqValid = 1 << 9,
  timeTempoValid = 1 << 10,
  timeBarsValid = 1 << 11,
  timeCyclePosValid = 1 << 12,
  timeSigValid = 1 << 13,
  timeClockValid = 1 << 15,
};

// Process levels (hostGetProcessLevel).
enum : int32_t { levelUser = 1, levelRealtime = 2, levelOffline = 4 };

// Output pin description (opGetOutputInfo).
struct PinInfo {
  char label[64];
  int32_t flags;  // 1 active, 2 first of a stereo pair
  int32_t arrangement;
  char shortLabel[8];
  char future[48];
};

}  // namespace gl::vst2abi

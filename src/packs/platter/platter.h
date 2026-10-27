// Gridline — "platter" pack: Platter (gl.platter), one deck of a DJ setup as a
// channel-rack instrument. Shared by the catalogue, the model, the runtime, the
// editor and the tests.
//
//   model     the beat grid (from the shared tempo analysis), the cue, eight hot
//             cues, the loop, the controller set-up (profile, side, device and an
//             editable binding list) and the recorded takes; stored as text in
//             PluginState::data after the audio file's path.
//   mapping   controller profiles (Pioneer DDJ-200 left / right, Hercules
//             DJControl Inpulse 200, Numark Mixtrack Pro FX, generic learn) and
//             the decoder that turns raw MIDI into deck controls.
//   runtime   the deck: platter physics (hand tracking, motor start / brake,
//             throws, nudges), band-limited reads at any speed and direction,
//             tempo sync through the beat grid, cue / hot cues / loops / slip /
//             key lock, the DJ EQ, filter, channel fader and crossfader, and the
//             take recorder / player.
//   hub       controller MIDI from the device driver threads to every deck, lock
//             free, whichever channel is selected.
//
// Takes: while Gridline records, an armed deck captures the exact path of its
// read head (position, level and flags every control step). The take is stored
// in the model and a note on the take's key is written into the pattern; that
// note replays the performance sample for sample.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/Types.h"
#include "../seltmann/seltmann.h"

namespace gl {
class Generator;
}

namespace gl::packs::platter {

inline constexpr const char* kType = "gl.platter";

// ------------------------------------------------------------------ parameters
enum Param {
  PVolume = 0,   // channel fader 0..1
  PTrim,         // dB -12..12
  PEqHigh,       // dB -26..6 (the bottom is a kill)
  PEqMid,
  PEqLow,
  PFilter,       // -1..1: low-pass to the left, high-pass to the right
  PFilterRes,    // 0..1
  PPitch,        // -1..1 pitch fader (times the range)
  PPitchRange,   // choice: 4 %, 8 %, 16 %, 50 %
  PSync,         // toggle: tempo locked to the project
  PKeyLock,      // toggle: the pitch stays when the tempo changes
  PQuantize,     // toggle: cues, loops and jumps snap to the beat
  PSlip,         // toggle: slip mode
  PVinyl,        // toggle: the platter top scratches (off: it nudges)
  PStartTime,    // s 0..3 motor spin-up
  PStopTime,     // s 0..3 brake
  PFeel,         // 0..1 hand tracking: loose .. tight
  PXfader,       // 0..1 crossfader (0 = side A)
  PXfSide,       // choice: Thru / A / B
  PXfCurve,      // 0..1 smooth (constant power) .. sharp (scratch cut)
  PFaderCurve,   // choice: Smooth / Linear / Sharp
  PNoteMode,     // choice: Gate / Trigger (notes on the hot cue keys)
  PLoopSize,     // choice: 1/8 .. 32 beats (auto loop and beat jump)
  PCount
};
enum PitchRange { Range4 = 0, Range8, Range16, Range50, RangeCount };
enum XfSide { XfThru = 0, XfA, XfB };
enum FaderCurve { FcSmooth = 0, FcLinear, FcSharp };
enum NoteMode { NoteGate = 0, NoteTrigger };
inline constexpr int kLoopSizes = 9;
double loopBeats(int index);        // 0.125 .. 32
const char* loopLabel(int index);   // "1/8" .. "32"
double pitchRangeOf(int range);     // 0.04 .. 0.5

inline constexpr int kHotCues = 8;
inline constexpr int kStep = 32;                // control step (output samples)
inline constexpr double kSecondsPerRev = 1.8;   // 33 1/3 rpm
inline constexpr int kHotKeyFirst = 60;         // notes 60..67 = hot cues 1..8
inline constexpr int kTakeKeyFirst = 36;        // notes 36..59 = takes 1..24
inline constexpr int kTakeKeys = 24;
inline constexpr int kMaxBindings = 192;

// EQ / fader values of a normalized controller position (EQ: centre = 0 dB, bottom = kill).
double eqDbOf(double norm);
double eqNormOf(double db);
inline constexpr double kEqKillDb = -26.0;

// ------------------------------------------------------------------ deck controls
enum Control : uint8_t {
  CtlNone = 0,
  CtlPlay, CtlCue, CtlSync, CtlKeyLock, CtlShift,
  CtlTouch, CtlJogPlatter, CtlJogRim, CtlJogSearch,
  CtlPitch, CtlVolume, CtlXfader, CtlEqHigh, CtlEqMid, CtlEqLow, CtlFilter, CtlTrim,
  CtlHot1, CtlHot2, CtlHot3, CtlHot4, CtlHot5, CtlHot6, CtlHot7, CtlHot8,
  CtlHotClear1, CtlHotClear2, CtlHotClear3, CtlHotClear4, CtlHotClear5, CtlHotClear6, CtlHotClear7, CtlHotClear8,
  CtlLoopIn, CtlLoopOut, CtlLoopAuto, CtlLoopHalve, CtlLoopDouble, CtlJumpBack, CtlJumpFwd,
  CtlSlip, CtlVinyl, CtlQuantize, CtlCensor, CtlJumpStart, CtlBendDown, CtlBendUp,
  CtlLoaded,  // output only (the "track loaded" lamp)
  CtlCount
};
enum ControlType : uint8_t { TypeButton = 0, TypeAbsolute, TypeRelative, TypeOutput };
struct ControlInfo {
  const char* id;     // stable text id (model, presets)
  const char* name;   // English name (the UI translates)
  ControlType type;
  const char* group;  // "Transport", "Jog", "Mixer", "Pads", "Loop", "Modes"
  int param;          // parameter it moves (absolute controls, mode toggles), -1 = none
};
const ControlInfo& controlInfo(int c);
int controlById(const std::string& id);  // CtlNone when unknown

// ------------------------------------------------------------------ mapping
enum MsgKind : uint8_t { MkNote = 0, MkCC, MkCC14 };
enum Encoding : uint8_t { EncAbs = 0, EncRelOffset, EncRelTwos, EncRelSign };
enum Layer : uint8_t { LayerAny = 0, LayerPlain, LayerShift };
struct Binding {
  uint8_t control = CtlNone;
  uint8_t kind = MkNote;
  int8_t channel = -1;   // 0..15, -1 = any
  uint8_t num = 0;       // note / CC (MSB for 14 bit)
  uint8_t lsb = 0;       // 14 bit: the LSB's CC
  uint8_t enc = EncAbs;
  bool invert = false;
  uint8_t layer = LayerAny;
  bool operator==(const Binding& o) const;
};
// LED feedback: `status` / `num` is sent with `on` or `off` when the source changes.
struct Led {
  uint8_t control = CtlNone;  // state source (play, cue, sync, hot cue k, loaded ...)
  uint8_t status = 0x90, num = 0, on = 0x7F, off = 0x00;
  bool operator==(const Led& o) const;
};
struct Mapping {
  std::string profile;        // profile id ("" = no controller)
  int side = 0;               // 0 left deck, 1 right deck
  std::string device;         // MIDI input name ("" = any device)
  double ticksPerRev = 128;   // jog resolution
  bool feedback = true;       // send LED feedback
  std::vector<Binding> bindings;
  std::vector<Led> leds;
  std::vector<std::vector<uint8_t>> init;  // sent once when the deck binds (queries, modes)
  bool operator==(const Mapping& o) const;
  bool empty() const { return bindings.empty(); }
};
struct Profile {
  const char* id;
  const char* name;        // shown in the set-up
  const char* match;       // device name substrings, "; " separated (auto detect)
  bool sides;              // has a left and a right deck
  const char* note;        // one line about it (shown in the set-up)
};
const std::vector<Profile>& profiles();
const Profile* findProfile(const std::string& id);
// The full mapping of a profile for one side (an empty mapping for "generic").
Mapping profileMapping(const std::string& id, int side);
// Profile id that matches a device name ("" = none).
std::string profileForDevice(const std::string& deviceName);
// "Note 0x0B ch 1", "CC 0x22 ch 1 relative", "CC 0x13 / 0x33 ch 1" (English, for the binding list).
std::string bindingText(const Binding& b);
// A binding learned from one message (absolute CC 7 bit, note, or a relative CC when `relative`).
Binding learnBinding(int control, uint8_t status, uint8_t d1, uint8_t d2, bool relative);
// Mapping preset files (<dir>/<name>.gpmap): the mapping as text.
std::string mappingToText(const Mapping& m);
bool mappingFromText(const std::string& text, Mapping& m);
bool saveMappingPreset(const std::string& dir, const std::string& name, const Mapping& m);
bool loadMappingPreset(const std::string& path, Mapping& m);
std::vector<std::string> mappingPresetFiles(const std::string& dir);  // full paths, sorted

// Input events of the decoder.
struct InEvent {
  uint8_t control = CtlNone;
  uint8_t type = TypeButton;
  bool down = false;    // buttons
  float value = 0;      // absolute: 0..1
  int ticks = 0;        // relative: signed detents
};
// Turns raw MIDI into deck controls with a mapping (no allocation; any thread
// that owns the decoder). 14 bit controls combine MSB and LSB.
class Decoder {
 public:
  void reset();
  // Returns how many events were written to out (at most max).
  int feed(const Mapping& m, uint8_t status, uint8_t d1, uint8_t d2, InEvent* out, int max);
  bool shift() const { return shift_; }

 private:
  std::array<uint8_t, kMaxBindings> msb_{};
  std::array<uint8_t, kMaxBindings> lsb_{};
  bool shift_ = false;
};

// ------------------------------------------------------------------ takes
enum TakeFlag : uint8_t { TfJump = 1, TfKeyLock = 2, TfMotor = 4, TfHand = 8 };
struct TakePoint {
  double pos = 0;    // read position at the end of the step (source frames)
  double from = 0;   // TfJump: where the step's motion starts (a ghost continues the old one)
  float gain = 1;    // fader x crossfader at the end of the step
  uint8_t flags = 0;
  bool operator==(const TakePoint& o) const;
};
struct Take {
  int key = kTakeKeyFirst;
  std::string name;
  double sampleRate = 44100;  // engine rate it was recorded at
  int step = kStep;           // output samples per point at that rate
  double tempo = 120;         // project tempo it was recorded at
  int ppq = 96;
  Tick startTick = 0, lengthTicks = 0;  // where it was recorded (song or pattern ticks)
  double startPos = 0;        // read position before the first point
  double startRate = 0;       // read speed before the first point (frames per sample)
  float startGain = 1;
  std::vector<TakePoint> pts;
  bool operator==(const Take& o) const;
  double seconds() const { return sampleRate > 0 ? static_cast<double>(pts.size()) * step / sampleRate : 0; }
};
// Compact text of the points (delta, zero-run and varint coded, base64).
std::string encodePoints(const std::vector<TakePoint>& pts);
bool decodePoints(const std::string& text, std::vector<TakePoint>& pts);
// Free take key (kTakeKeyFirst.., -1 when all are used).
int freeTakeKey(const std::vector<Take>& takes);

// ------------------------------------------------------------------ model
struct Model {
  int64_t srcFrames = 0;
  double srcRate = 0;
  bool analysed = false;         // the grid comes from the analysis (else a flat 120 BPM grid)
  seltmann::Analysis grid;       // beats, bpm, beatsPerBar, bar1 (the rest is not stored)
  double cue = 0;                // main cue (source frame)
  std::array<double, kHotCues> hot{};  // source frames, < 0 = not set
  double loopIn = -1, loopOut = -1;    // the last loop (frames, -1 none)
  bool asked = false;            // the controller question was answered
  bool armed = true;             // records takes while Gridline records
  Mapping map;
  std::vector<Take> takes;
  Model() { hot.fill(-1); }
  bool operator==(const Model& o) const;
};
// "<path>\n<model>" <-> path + model. A bare path yields hasModel = false.
std::string pathOf(const std::string& data);
bool parseData(const std::string& data, std::string& path, Model& m);
std::string formatData(const std::string& path, const Model& m);
std::string modelToText(const Model& m);
bool modelFromText(const std::string& text, Model& m);
// A model for a sample and its analysis (grid, cue on the first downbeat).
Model defaultModel(const Sample& s, const seltmann::Analysis& a, bool analysed);
// Keeps the controller set-up of `keep`, the rest from `fresh` (a new file; takes stay with the old file).
Model carryOver(const Model& keep, const Model& fresh);
// Beat helpers over the stored grid.
double beatAt(const Model& m, double frame);
double frameAt(const Model& m, double beat);
double nearestBeatFrame(const Model& m, double frame);
double bpmOf(const Model& m);

// ------------------------------------------------------------------ hub (controller MIDI)
namespace hub {
// FNV-1a of a device name (0 = any / unknown).
uint32_t deviceId(const std::string& name);
// Driver threads (MidiManager): every short message of every input.
void push(uint32_t device, uint8_t status, uint8_t d1, uint8_t d2);
// The app's MIDI manager feeds the hub (decks then read devices from it; without
// a tap, as in the plugin builds, decks take their channel's MIDI instead).
void setTapped(bool on);
bool tapped();
uint64_t head();
// Message `index` (head() - size() .. head() - 1); false when it was overwritten.
bool read(uint64_t index, uint32_t& device, uint8_t& status, uint8_t& d1, uint8_t& d2);
inline constexpr uint64_t kSize = 4096;
// Devices bound to a deck (UI thread sets them every frame): their notes do not
// play the selected instrument and the app's templates and links leave them alone.
inline constexpr int kClaims = 8;
void setClaimed(const uint32_t* ids, int count);
bool claimed(uint32_t id);
}  // namespace hub

// ------------------------------------------------------------------ live view (UI <- audio)
struct LiveView {
  bool valid = false;
  bool loaded = false;
  double pos = 0;           // read position (source frames)
  double speed = 0;         // read speed relative to the file's own speed (1 = original)
  double angle = 0;         // platter angle (turns)
  bool playing = false;     // motor on
  bool touched = false;     // a hand is on the platter (controller or mouse)
  bool slipping = false;
  double slipPos = -1;
  bool loopOn = false;
  double loopIn = -1, loopOut = -1;
  double cue = 0;
  bool cueLit = false;
  std::array<double, kHotCues> hot{};
  bool sync = false, keylock = false, slip = false, vinyl = true, quantize = true;
  bool keylockActive = false;  // the key lock engine is in use right now
  double tempo = 0;            // effective BPM
  int take = -1;               // take key playing (-1 none)
  bool capturing = false;      // recording a take
  float level = 0;             // output peak of the last block
  bool shift = false;
  uint32_t rawSeq = 0;         // last raw MIDI message the deck saw (learn)
  uint8_t raw[3] = {0, 0, 0};
};
bool liveView(const Generator* g, LiveView& out);

// Controller-moved values for the editor to write into the parameters (and the
// automation recorder). `seq` grows when the controller moved it.
struct Mirror {
  std::array<uint32_t, PCount> seq{};
  std::array<float, PCount> value{};
};
bool mirror(const Generator* g, Mirror& out);
// Hot cues, cue and loop the deck set itself (controller): the editor saves them.
struct Marks {
  uint32_t seq = 0;
  double cue = 0;
  std::array<double, kHotCues> hot{};
  double loopIn = -1, loopOut = -1;
};
bool marks(const Generator* g, Marks& out);

// Commands from the editor (UI thread) to the deck.
enum CmdKind : uint8_t {
  CmdButton = 0,   // control a, down = v != 0
  CmdHandDown,     // mouse on the platter (grabs the record where it is)
  CmdHandMove,     // v = platter turns since the hand went down
  CmdHandUp,
  CmdSeek,         // v = frame
  CmdLoop,         // v = in, w = out (in < 0: loop off)
  CmdAbsolute,     // control a, v = 0..1
};
struct Cmd {
  uint8_t kind = CmdButton;
  uint8_t a = 0;
  double v = 0, w = 0;
};
bool command(Generator* g, const Cmd& c);

// Recorded material handed from the deck to the editor (UI thread drains it).
struct CaptureEvent {
  enum Kind : uint8_t { Start = 0, Point, End, Lost } kind = Point;
  TakePoint pt;
  double tick = 0;        // Start / End: tick (song or pattern)
  double tempo = 0, sampleRate = 0, startRate = 0;
  int ppq = 0;
  float gain = 1;
};
// Takes up to `max` events; returns how many.
int drainCapture(Generator* g, CaptureEvent* out, int max);
// Builds takes from the capture events (UI thread).
struct TakeBuilder {
  bool active = false, lost = false;
  Take take;
  // True when an End completed a take (in `take`).
  bool feed(const CaptureEvent& e);
};

}  // namespace gl::packs::platter

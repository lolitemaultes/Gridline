// Gridline — "platter" pack: Platter (gl.platter), a DJ setup as a channel-rack
// instrument: two decks and a mixer between them (or one deck, as a layout).
// Shared by the catalogue, the model, the runtime, the editor and the tests.
//
//   model     per deck: the track (an entry of the model's file list), the beat
//             grid (from the shared tempo analysis), the cue, eight hot cues and
//             the last loop; the layout, the controller set-up (profile, device
//             and an editable binding list whose entries target deck A, deck B
//             or the mixer) and the recorded takes. Stored as text in
//             PluginState::data after deck A's track path (the first line).
//   mapping   controller profiles (Pioneer DDJ-200, Hercules DJControl Inpulse
//             200, Numark Mixtrack Pro FX: both sides at once, or one side on
//             one deck; generic learn) and the decoder that turns raw MIDI into
//             deck and mixer controls.
//   runtime   the decks: platter physics (scratching after the mechanics of Mixxx:
//             jog ticks filtered into a speed, the mouse through a position loop;
//             motor start / brake, throws, nudges), band-limited reads at any speed and direction,
//             tempo sync through the beat grid (to the project or to the other
//             deck), cue / hot cues / loops / slip / key lock, the DJ EQ and
//             filter per channel; the mixer: channel faders, crossfader (curve,
//             cut, reverse), master; the take recorder / player.
//   hub       controller MIDI from the device driver threads to every deck, lock
//             free, whichever channel is selected.
//
// Takes: while Gridline records, an armed setup captures the exact path of both
// read heads (position, level and flags every control step) and the mixer moves
// that are not in those levels. The take is stored in the model (with the files
// it was played on) and a note on the take's key is written into the pattern;
// that note replays the performance sample for sample.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/Song.h"
#include "../../core/Types.h"
#include "../seltmann/seltmann.h"

namespace gl {
class Generator;
class Document;
}

namespace gl::packs::platter {

inline constexpr const char* kType = "gl.platter";
inline constexpr int kDecks = 2;

// ------------------------------------------------------------------ parameters
// One deck's parameters (the order of deck B's block).
enum DeckParam {
  DVolume = 0,  // channel fader 0..1
  DTrim,        // dB -12..12
  DEqHigh,      // dB -26..6 (the bottom is a kill)
  DEqMid,
  DEqLow,
  DFilter,      // -1..1: low-pass to the left, high-pass to the right
  DFilterRes,   // 0..1
  DPitch,       // -1..1 pitch fader (times the range)
  DPitchRange,  // choice: 4 %, 8 %, 16 %, 50 %
  DSync,        // toggle: tempo locked (to the project or the other deck)
  DKeyLock,     // toggle: the pitch stays when the tempo changes
  DQuantize,    // toggle: cues, loops and jumps snap to the beat
  DSlip,        // toggle: slip mode
  DVinyl,       // toggle: the platter top scratches (off: it nudges)
  DStartTime,   // s 0..3 motor spin-up
  DStopTime,    // s 0..3 brake
  DFeel,        // 0..1 hand feel: loose .. tight (0.5: the scratch filters at their standard speed)
  DXfSide,      // choice: Thru / A / B (crossfader assign)
  DLoopSize,    // choice: 1/8 .. 32 beats (auto loop and beat jump)
  DCount,       // (deck B's block: PDeckB .. PDeckB + DCount - 1)
  // Added after the block (J-154); their ids follow the mixer's, see deckParam.
  DJog = DCount,  // 0.25..2 jog sensitivity: record travel per jog tick (1 = one turn per turn)
  // J-159: what a recorded performance plays back besides the controls (lanes in the
  // pattern, see "performances"); the decks never move them themselves.
  DPos,    // 0..1 record position: a change makes the deck jump there (a seek, a search)
  DMouse,  // turns -1..1 (wraps): the mouse hand's travel since it grabbed the record
  DWheel,  // turns -1..1 (wraps): the travel the jog wheel gave the record, summed
  DNudge,  // wheel ticks -4096..4096 (wraps): rim nudges, summed
  DAll
};
// Parameter ids. 0..22 are J-150's single deck (deck A and the mixer values it
// had); deck B's block and the new mixer values follow (ids never move).
enum Param {
  PVolume = 0,  // deck A ...
  PTrim,
  PEqHigh,
  PEqMid,
  PEqLow,
  PFilter,
  PFilterRes,
  PPitch,
  PPitchRange,
  PSync,
  PKeyLock,
  PQuantize,
  PSlip,
  PVinyl,
  PStartTime,
  PStopTime,
  PFeel,
  PXfader,      // mixer: crossfader 0..1 (0 = side A)
  PXfSide,      // deck A: crossfader assign
  PXfCurve,     // mixer: 0..1 smooth (constant power) .. sharp (scratch cut)
  PFaderCurve,  // mixer: channel fader curve, Smooth / Linear / Sharp
  PNoteMode,    // mixer: Gate / Trigger (notes on the hot cue keys)
  PLoopSize,    // deck A: loop size
  PDeckB,       // 23: deck B's block, DeckParam order
  PSyncTo = PDeckB + DCount,  // mixer: Project / Other deck
  PMaster,      // mixer: master level, dB -40..6 (the bottom is off)
  PXfCut,       // mixer: crossfader cut mode (the sharpest curve)
  PXfReverse,   // mixer: crossfader reverse
  PJogA,        // 46 (J-154): deck A jog sensitivity
  PJogB,        // deck B jog sensitivity
  PPosA,        // 48 (J-159): performance lanes, deck A then deck B for each
  PPosB,
  PMouseA,
  PMouseB,
  PWheelA,
  PWheelB,
  PNudgeA,
  PNudgeB,
  PCount
};
// The parameter id of deck `deck`'s parameter `d`.
constexpr int deckParam(int deck, int d) {
  if (d == DJog) return deck == 0 ? static_cast<int>(PJogA) : static_cast<int>(PJogB);
  if (d >= DPos) return static_cast<int>(PPosA) + (d - DPos) * 2 + (deck & 1);
  return deck == 0 ? (d < DXfSide ? d : d == DXfSide ? static_cast<int>(PXfSide) : static_cast<int>(PLoopSize)) : static_cast<int>(PDeckB) + d;
}
inline constexpr double kJogSensDefault = 0.8;  // a little less than one turn per turn
// Deck (0 / 1) of a parameter id and its DeckParam in `d`; -1 for mixer parameters.
int deckOfParam(int p, int& d);

enum PitchRange { Range4 = 0, Range8, Range16, Range50, RangeCount };
enum XfSide { XfThru = 0, XfA, XfB };
enum FaderCurve { FcSmooth = 0, FcLinear, FcSharp };
enum NoteMode { NoteGate = 0, NoteTrigger };
// DMouse / DWheel range (turns either way). The lanes wrap at the ends (a hand that goes on
// turning starts again at the other end): a scratch fills a good part of the lane's height.
inline constexpr double kTravelTurns = 1;
inline constexpr double kNudgeSpan = 4096;  // DNudge range (wheel ticks either way)
// The performance parameters (DPos .. DNudge of both decks): played from lanes, never
// moved by the decks themselves.
constexpr bool perfParam(int p) { return p >= PPosA && p <= PNudgeB; }
enum SyncTo { SyncProject = 0, SyncDeck };
inline constexpr int kLoopSizes = 9;
double loopBeats(int index);        // 0.125 .. 32
const char* loopLabel(int index);   // "1/8" .. "32"
double pitchRangeOf(int range);     // 0.04 .. 0.5
inline constexpr double kMasterOffDb = -40.0;

inline constexpr int kHotCues = 8;
inline constexpr int kStep = 32;                // control step (output samples)
inline constexpr double kSecondsPerRev = 1.8;   // 33 1/3 rpm
inline constexpr int kHotKeyFirst = 60;         // notes 60..67 = deck A hot cues 1..8
inline constexpr int kHotKeyFirstB = 72;        // notes 72..79 = deck B hot cues 1..8
inline constexpr int kTakeKeyFirst = 36;        // notes 36..59 = takes 1..24
inline constexpr int kTakeKeys = 24;
// J-159: command keys. A note on one holds a deck's button for as long as it lasts
// (recorded performances write them): deck A 80..102, deck B 104..126; the hot cue
// clears on 20..27 (A) and 28..35 (B). Key 68 starts a recorded performance: the decks
// go back to how they were when it was recorded (the setup keeps that state; the
// note's mod X / mod Y say which).
enum CmdKey {
  KeyPlay = 0,
  KeyCue,
  KeyHot1,  // .. KeyHot1 + 7: hot cues (set on an empty pad, else trigger)
  KeyLoopIn = KeyHot1 + kHotCues,
  KeyLoopOut,
  KeyLoopAuto,
  KeyLoopHalve,
  KeyLoopDouble,
  KeyJumpBack,
  KeyJumpFwd,
  KeyJumpStart,
  KeyCensor,
  KeyBendDown,
  KeyBendUp,
  KeyTouch,  // the jog wheel's top is touched
  KeyMouse,  // the mouse holds the record (its travel: the mouse travel lane)
  KeyClear1,  // .. KeyClear1 + 7: hot cue clears (their own key range)
  kCmdKeys = KeyClear1 + kHotCues
};
inline constexpr int kCmdKeyFirstA = 80;
inline constexpr int kCmdKeyFirstB = 104;
inline constexpr int kClearKeyFirstA = 20;
inline constexpr int kClearKeyFirstB = 28;
inline constexpr int kStartKey = 68;
// MIDI note of command key `index` on deck `deck`.
int cmdKeyPitch(int deck, int index);
// Deck (0 / 1) and CmdKey of a note, -1 when it is not a command key.
int cmdKeyOf(int pitch, int& index);
// The deck control a command key presses (CtlNone for KeyMouse), and the key of a control (-1 none).
int cmdKeyControl(int index);
int controlCmdKey(int control);
// Name of a Platter key for the piano roll ("A Cue", "B Hot cue 3", "Take 2"; "" for plain keys).
std::string keyName(int pitch);
// The id a start note carries in its mod X / mod Y (0..65535) and back.
inline int startIdOf(int modX, int modY) { return modX + 256 * modY; }
// A command key note with this mod Y holds a button that was already down when its pass
// began: on the sample a performance starts its press does nothing (the start state has the
// button down), only its release acts; anywhere else it presses as usual.
inline constexpr int kHeldModY = 0;
inline constexpr int kMaxBindings = 256;

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
  CtlMaster,  // mixer: master level
  CtlCount
};
enum ControlType : uint8_t { TypeButton = 0, TypeAbsolute, TypeRelative, TypeOutput };
struct ControlInfo {
  const char* id;     // stable text id (model, presets)
  const char* name;   // English name (the UI translates)
  ControlType type;
  const char* group;  // "Transport", "Jog", "Mixer", "Pads", "Loop", "Modes"
  int param;          // DeckParam it moves (deck controls) or Param (mixer controls), -1 = none
  bool mixer;         // a mixer control (one for both decks)
};
const ControlInfo& controlInfo(int c);
int controlById(const std::string& id);  // CtlNone when unknown
// The parameter id a control moves on `deck` (-1 none).
int controlParam(int c, int deck);

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
  uint8_t deck = 0;      // 0 deck A, 1 deck B (mixer controls ignore it)
  bool operator==(const Binding& o) const;
};
// LED feedback: `status` / `num` is sent with `on` or `off` when the source changes.
struct Led {
  uint8_t control = CtlNone;  // state source (play, cue, sync, hot cue k, loaded ...)
  uint8_t status = 0x90, num = 0, on = 0x7F, off = 0x00;
  uint8_t deck = 0;
  bool operator==(const Led& o) const;
};
inline constexpr int kSideBoth = 2;  // Mapping::side: the whole controller (deck 1 -> A, deck 2 -> B)
struct Mapping {
  std::string profile;        // profile id ("" = no controller)
  int side = kSideBoth;       // 0 left deck, 1 right deck (on deck A), kSideBoth both
  std::string device;         // MIDI input name ("" = any device)
  double ticksPerRev = 128;   // jog resolution
  bool feedback = true;       // send LED feedback
  std::vector<Binding> bindings;
  std::vector<Led> leds;
  std::vector<std::vector<uint8_t>> init;  // sent once when the setup binds (queries, modes)
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
// The full mapping of a profile: one side (0 / 1) on deck A, or kSideBoth (deck 1
// on A, deck 2 on B, the mixer once). An empty mapping for "generic".
Mapping profileMapping(const std::string& id, int side);
// Profile id that matches a device name ("" = none).
std::string profileForDevice(const std::string& deviceName);
// The jog scratch filter of a profile: alpha / beta of the tick-rate filter per
// millisecond (the values controller mappings for Mixxx use for these wheels;
// any other profile gets the Pioneer / Hercules pair).
struct JogFilter {
  double alpha = 1.0 / 8;
  double beta = 1.0 / 256;
};
JogFilter jogFilterOf(const std::string& profile);
// "Note 0x0B ch 1", "CC 0x22 ch 1 relative", "CC 0x13 / 0x33 ch 1" (English, for the binding list).
std::string bindingText(const Binding& b);
// A binding learned from one message (absolute CC 7 bit, note, or a relative CC when `relative`).
Binding learnBinding(int control, uint8_t status, uint8_t d1, uint8_t d2, bool relative, int deck = 0);
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
  uint8_t deck = 0;     // target deck (mixer controls: 0)
  bool down = false;    // buttons
  float value = 0;      // absolute: 0..1
  int ticks = 0;        // relative: signed detents
};
// Turns raw MIDI into deck and mixer controls with a mapping (no allocation;
// any thread that owns the decoder). 14 bit controls combine MSB and LSB.
// Shift is held per deck.
class Decoder {
 public:
  void reset();
  // Returns how many events were written to out (at most max).
  int feed(const Mapping& m, uint8_t status, uint8_t d1, uint8_t d2, InEvent* out, int max);
  bool shift(int deck = 0) const { return shift_[deck & 1]; }

 private:
  std::array<uint8_t, kMaxBindings> msb_{};
  std::array<uint8_t, kMaxBindings> lsb_{};
  bool shift_[2] = {false, false};
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
// One deck's part of a take.
struct TakeStream {
  bool on = false;
  int file = -1;              // Model::files index of the track it was played on
  double startPos = 0;        // read position before the first point
  double startRate = 0;       // read speed before the first point (frames per sample)
  float startGain = 1;
  std::vector<TakePoint> pts;
  bool operator==(const TakeStream& o) const;
};
// A mixer value that changed at a step of the take (trim, EQ, filter, master).
struct TakeMove {
  uint32_t step = 0;
  uint8_t param = 0;  // Param id
  float value = 0;
  bool operator==(const TakeMove& o) const { return step == o.step && param == o.param && value == o.value; }
};
struct Take {
  int key = kTakeKeyFirst;
  std::string name;
  double sampleRate = 44100;  // engine rate it was recorded at
  int step = kStep;           // output samples per point at that rate
  double tempo = 120;         // project tempo it was recorded at
  int ppq = 96;
  Tick startTick = 0, lengthTicks = 0;  // where it was recorded (song or pattern ticks)
  std::array<TakeStream, kDecks> s;
  std::vector<TakeMove> moves;          // sorted by step
  bool operator==(const Take& o) const;
  size_t steps() const;
  double seconds() const { return sampleRate > 0 ? static_cast<double>(steps()) * step / sampleRate : 0; }
};
// Parameters a take records as moves.
inline constexpr int kMoveParams = 13;
const std::array<uint8_t, kMoveParams>& moveParams();
// Compact text of the points (delta, zero-run and varint coded, base64).
std::string encodePoints(const std::vector<TakePoint>& pts);
bool decodePoints(const std::string& text, std::vector<TakePoint>& pts);
std::string encodeMoves(const std::vector<TakeMove>& mv);
bool decodeMoves(const std::string& text, std::vector<TakeMove>& mv);
// Free take key (kTakeKeyFirst.., -1 when all are used).
int freeTakeKey(const std::vector<Take>& takes);

// ------------------------------------------------------------------ model
struct DeckModel {
  int file = -1;                 // Model::files index of the track (-1 = empty deck)
  int64_t srcFrames = 0;
  double srcRate = 0;
  bool analysed = false;         // the grid comes from the analysis (else a flat 120 BPM grid)
  seltmann::Analysis grid;       // beats, bpm, beatsPerBar, bar1, key (the rest is not stored)
  double cue = 0;                // main cue (source frame)
  std::array<double, kHotCues> hot{};  // source frames, < 0 = not set
  double loopIn = -1, loopOut = -1;    // the last loop (frames, -1 none)
  DeckModel() { hot.fill(-1); }
  bool operator==(const DeckModel& o) const;
};
enum Layout { LayoutOne = 1, LayoutTwo = 2 };
// J-159: what a recording pass becomes.
enum RecordAs { RecAutomation = 0, RecTake };
// J-159: how the decks were when a performance was recorded (restored by its start note).
// The values are the runtime's own (opaque here); stored exactly (hex) in the model.
struct PerfStart {
  int id = 0;
  std::vector<double> v;
  bool operator==(const PerfStart& o) const { return id == o.id && v.size() == o.v.size() && std::equal(v.begin(), v.end(), o.v.begin(), [](double a, double b) { return a == b || (a != a && b != b); }); }
};
struct Model {
  std::vector<std::string> files;  // every track the model refers to (decks, takes)
  std::array<DeckModel, kDecks> deck;
  int layout = LayoutTwo;
  bool asked = false;            // the controller question was answered
  bool armed = true;             // records while Gridline records
  int recordAs = RecAutomation;  // a performance in the pattern (lanes and command keys), or a take
  Mapping map;
  std::vector<Take> takes;
  std::vector<PerfStart> starts;  // J-159: recorded performances' start states
  bool operator==(const Model& o) const;
  bool twoDecks() const { return layout != LayoutOne; }
  // The track of a deck ("" = empty).
  std::string trackOf(int d) const;
};
// "<path>\n<model>" <-> path + model. A bare path yields hasModel = false (the
// model then holds deck A's file only). Deck A's file is always the first line:
// a different first line (a drop on the channel) is deck A's new track.
std::string pathOf(const std::string& data);
bool parseData(const std::string& data, std::string& path, Model& m);
std::string formatData(const std::string& path, const Model& m);
std::string modelToText(const Model& m);
bool modelFromText(const std::string& text, Model& m);
// File list index of `path` (added when missing).
int fileIndex(Model& m, const std::string& path);
// Drops files no deck and no take refers to (indices are renumbered).
void pruneFiles(Model& m);
// A deck model for a sample and its analysis (grid, cue on the first downbeat).
DeckModel deckModel(const Sample& s, const seltmann::Analysis& a, bool analysed);
// Puts `path` on deck `d` with `fresh` (its grid and cues); the rest of the model stays.
void loadTrack(Model& m, int d, const std::string& path, const DeckModel& fresh);
// Beat helpers over a deck's stored grid.
double beatAt(const DeckModel& m, double frame);
double frameAt(const DeckModel& m, double beat);
double nearestBeatFrame(const DeckModel& m, double frame);
double bpmOf(const DeckModel& m);

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
// `ns` (when given): when it arrived (steady clock, nanoseconds; see nowNs).
bool read(uint64_t index, uint32_t& device, uint8_t& status, uint8_t& d1, uint8_t& d2, int64_t* ns = nullptr);
// The steady clock the hub stamps messages with (nanoseconds).
int64_t nowNs();
inline constexpr uint64_t kSize = 4096;
// Devices bound to a setup (UI thread sets them every frame): their notes do not
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
  bool following = false;      // synced to the other deck right now
  double tempo = 0;            // effective BPM
  double beat = 0;             // beat position at pos (the deck's grid)
  int take = -1;               // take key playing (-1 none)
  bool capturing = false;      // recording a take (or a performance)
  bool replaying = false;      // J-159: a recorded performance drives the decks
  float pre = 0;               // channel level before the fader (peak of the last block)
  float level = 0;             // channel level after fader and crossfader
  bool shift = false;
  uint32_t rawSeq = 0;         // last raw MIDI message the setup saw (learn)
  uint8_t raw[3] = {0, 0, 0};
};
bool liveView(const Generator* g, int deck, LiveView& out);
// J-159 (tests): what the decks use right now: the running step's parameter values, each
// deck's read position and speed, the setup's sample clock. Audio thread state: read it
// between blocks only (never while the engine runs on another thread).
struct StepState {
  std::array<float, PCount> p{};
  double pos[kDecks] = {0, 0}, vel[kDecks] = {0, 0};
  uint64_t clock = 0;
};
bool stepState(const Generator* g, StepState& out);
inline bool liveView(const Generator* g, LiveView& out) { return liveView(g, 0, out); }
// The mixer's live values.
struct MixView {
  bool valid = false;
  float masterL = 0, masterR = 0;  // output peaks of the last block
  bool transport = false;          // the project plays
  double songBeat = 0;             // the project's beat at the end of the last block
  double tempo = 120;              // project tempo
};
bool mixView(const Generator* g, MixView& out);

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
bool marks(const Generator* g, int deck, Marks& out);

// Commands from the editor (UI thread) to a deck.
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
  uint8_t deck = 0;
  double v = 0, w = 0;
};
bool command(Generator* g, const Cmd& c);

// Recorded material handed from the setup to the editor (UI thread drains it).
// J-159: a performance capture adds Move events for every parameter (the normalized
// value the decks used, perf lanes included: value = lane value), Button events (deck,
// CmdKey, down) and the start state in Snap events (8 values each, in order).
inline constexpr int kSnapPer = 8;
struct CaptureEvent {
  enum Kind : uint8_t { Start = 0, Point, Move, End, Lost, Button, Snap } kind = Point;
  uint8_t streams = 0;    // Start: decks with a track (bit d)
  uint8_t param = 0;      // Move: Param id; Button: CmdKey
  uint8_t deck = 0;       // Button
  float value = 0;        // Move (normalized for a performance, plain for a take); Button: 1 down, 0 up
  uint32_t step = 0;      // Move / Button: the step it acts on; Snap: values [step * kSnapPer, ...)
  TakePoint pt[kDecks];   // Point: both decks; Start: pt[d].pos = start position
  double snap[kSnapPer] = {};  // Snap: start state values; Start: snap[0] = their count, snap[1] = the start tick (exact)
  int file[kDecks] = {-1, -1};      // Start
  double startRate[kDecks] = {0, 0};  // Start
  float gain[kDecks] = {1, 1};      // Start
  double tick = 0;        // Start / End: tick (song or pattern)
  double tempo = 0, sampleRate = 0;
  int ppq = 0;
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

// ------------------------------------------------------------------ performances (J-159)
// A pass recorded as pattern data: a start note, notes on the command keys and lanes on
// the parameters that moved (the performance lanes of both decks included). Times are
// exact ticks (fractions kept): every event sits where it acts on the step it acted on
// live, so the replay is the same sample for sample.
struct PerfNote {
  double on = 0, off = 0;  // ticks
  int key = 0;
  bool held = false;  // down before the pass began (kHeldModY)
};
struct PerfPoint {
  double tick = 0;
  float value = 0;  // normalized
};
struct PerfLane {
  int param = 0;  // Param id
  std::vector<PerfPoint> points;
};
struct Performance {
  double startTick = 0;  // where the pass started (song or pattern ticks, exact)
  double endTick = 0;
  std::vector<PerfNote> notes;
  std::vector<PerfLane> lanes;
  std::vector<double> start;  // the start state (Model::starts entry)
  const PerfLane* lane(int param) const;
};
// Builds performances from the capture events (UI thread). Lanes are lossless: one Hold
// point wherever the value the decks used changed.
class PerformanceBuilder {
 public:
  bool active = false, lost = false;
  Performance perf;  // after an End
  // True when an End completed a performance (in `perf`).
  bool feed(const CaptureEvent& e);
  // The pass's start tick (valid while active).
  double startTick() const { return tick0_; }

 private:
  double tick0_ = 0, tps_ = 0;  // start tick (exact), ticks per step
  size_t snapN_ = 0;
  uint32_t steps_ = 0;
  std::vector<std::pair<uint32_t, float>> moves_[PCount];
  struct Btn {
    uint32_t step;
    uint8_t deck, key;
    bool down;
  };
  std::vector<Btn> btns_;
  void build(double endTick);
};

// The pattern-data form of a performance at pattern tick `origin` (the pass start lands
// on origin + its fraction): notes on `channel`'s keys and lane points (sub-tick frac).
// `startId`: the start note's id (Model::starts).
void performanceNotes(const Performance& perf, double origin, int startId, std::vector<Note>& out);
std::vector<AutoPoint> performancePoints(const PerfLane& lane, double origin);

// Pattern helpers (UI thread; edits go through the Document, so they are undoable).
// A pattern holds a Platter performance of `channel`: notes on its command, start or take
// keys, or a lane on one of its parameters.
bool holdsPerformance(const Pattern& p, ObjectId channel);
// The pattern a pass records into: `want` while it holds no performance of `channel`
// (an empty or unrelated pattern is used as is); else a new pattern "<channel> take N"
// after it. Pattern mode (`song` false): the new one has the old one's length and its
// other content (other channels, the channel's own hot key notes), so the loop and the
// backing play on; song mode: it starts empty.
ObjectId passPattern(Document& doc, ObjectId want, ObjectId channel, bool song);
// Song mode: the pattern-tick origin of the clip of `pattern` at song tick `tick`; a clip
// is placed on the bar (the channel's instrument track or a free track) when none covers it.
Tick clipOrigin(Document& doc, ObjectId pattern, ObjectId channel, Tick tick, Tick length);
// Writes `perf` (song or pattern ticks; `origin` = the pattern's tick 0 there) into
// `pattern`: notes, lanes, and the start state into the channel's model (its id).
// Song mode: the pattern grows to hold it.
void writePerformance(Document& doc, ObjectId pattern, ObjectId channel, double origin, const Performance& perf, bool song);

}  // namespace gl::packs::platter

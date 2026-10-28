// Gridline — song model: channels, patterns, playlist, mixer.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>
#include "PluginInfo.h"
#include "Types.h"

namespace gl {

using Rgb = uint32_t;  // 0xRRGGBB

// Channel echo ("note delay"): every note is repeated `count` times, each
// `time` beats after the previous one, with its velocity multiplied by
// `feedback`, its pitch moved by `pitch` cents and its pan set to `pan`
// (alternating sides with ping-pong). Off while count or feedback is 0.
struct ChannelEcho {
  int count = 0;            // 0..10
  float feedback = 0.5f;    // velocity factor per echo, 0..1
  float time = 0.375f;      // beats between echoes
  float pan = 0;            // -1..1
  int pitch = 0;            // cents per echo, -1200..1200
  bool pingPong = false;
  float modX = 0, modY = 0;  // note mod X / Y change per echo, -1..1 (0.5 = half the range)
  bool active() const { return count > 0 && feedback > 0; }
  bool operator==(const ChannelEcho& o) const {
    return count == o.count && feedback == o.feedback && time == o.time && pan == o.pan && pitch == o.pitch &&
           pingPong == o.pingPong && modX == o.modX && modY == o.modY;
  }
};

inline constexpr int kMaxCutGroups = 99;
inline constexpr int kMaxLayerChildren = 32;

// A channel-rack channel: an instrument plugin plus its strip and routing.
struct Channel {
  ObjectId id = kNoId;
  std::string name = "Channel";
  Rgb color = 0x5A8FD8;
  PluginState plugin;           // generator type + parameters (+ sample path)
  float gain = 0.78f;           // linear 0..1.25   (ChannelParam::Volume)
  float pan = 0.0f;             // -1..1            (ChannelParam::Pan)
  bool muted = false;           //                  (ChannelParam::Mute)
  int pitch = 0;                // semitones -24..24 (ChannelParam::Pitch)
  ObjectId mixerTrack = kNoId;  // MixerTrack::id; kNoId = master
  // Note processors run on this channel's notes before the instrument
  // (arpeggiator, chords, key mapping, …), in order. PluginKind::NoteFx types.
  std::vector<PluginState> noteFx;
  // --- Channel note settings (defaults keep the previous behaviour) ---
  int fine = 0;              // fine tune, cents -200..200 (added to `pitch`)
  float swingMix = 1.0f;     // share of the song swing applied to this channel's steps, 0..1
  float timeShift = 0;       // every note starts this many beats later, 0..1
  int polyphony = 0;         // maximum sounding notes (0 = unlimited); the oldest is cut
  bool mono = false;         // one note at a time (a new note replaces the sounding one)
  bool porta = false;        // notes glide from the previous note's pitch
  float portaTime = 0.1f;    // glide time for porta / mono legato, seconds 0..2
  int cutGroup = 0;          // starting a note cuts channels whose cutBy equals this (0 = none)
  int cutBy = 0;             // this channel is cut by notes of channels in this group (0 = none)
  ChannelEcho echo;
  // Layer: notes of this channel also play on these channels (the layer's own
  // instrument, if any, plays too). layerMode 0 = all children, 1 = one random child per note.
  std::vector<ObjectId> layer;
  int layerMode = 0;
  int group = -1;            // display group (index into Song::channelGroups), -1 = none
  // Multi-output instruments (hosted plugins): mixer track of each extra
  // output (output 2, 3, …); kNoId or missing = that output is not used.
  std::vector<ObjectId> auxOuts;
  // MIDI ports (hosted plugins): notes / MIDI a plugin sends on its output
  // port play on every channel whose input port matches (arpeggiators,
  // chord generators). -1 = none; 0..255.
  int midiOutPort = -1;
  int midiInPort = -1;
  // J-31: key range (MIDI notes, inclusive): notes outside it do not play on
  // this channel (layer splits use it). Swing truncation: swung steps end at
  // their unswung end (they do not overlap the next step).
  int keyLo = 0, keyHi = 127;
  bool swingTruncate = false;
  // J-39: the channel is a playlist audio-clip source (FL's audio-clip
  // channel), kept as such when no clip uses it.
  bool audioClip = false;
  // J-39 (FL interop): FL's icon number for the channel (0 = none); kept
  // for export, not drawn (Gridline's icons are its own).
  int flIcon = 0;
  bool fullKeyRange() const { return keyLo <= 0 && keyHi >= 127; }

  bool isLayer() const { return !layer.empty(); }
  bool operator==(const Channel& o) const {
    return id == o.id && name == o.name && color == o.color && plugin == o.plugin && gain == o.gain &&
           pan == o.pan && muted == o.muted && pitch == o.pitch && mixerTrack == o.mixerTrack && noteFx == o.noteFx &&
           fine == o.fine && swingMix == o.swingMix && timeShift == o.timeShift && polyphony == o.polyphony &&
           mono == o.mono && porta == o.porta && portaTime == o.portaTime && cutGroup == o.cutGroup &&
           cutBy == o.cutBy && echo == o.echo && layer == o.layer && layerMode == o.layerMode && group == o.group &&
           auxOuts == o.auxOuts && midiOutPort == o.midiOutPort && midiInPort == o.midiInPort && keyLo == o.keyLo &&
           keyHi == o.keyHi && swingTruncate == o.swingTruncate && audioClip == o.audioClip && flIcon == o.flIcon;
  }
};

// --- arrangement: automation curves (shared by automation clips and pattern automation)
// Curve of the segment that ends at an automation point.
enum class AutoShape : uint8_t {
  Curve = 0,  // single curve bent by the tension
  SCurve,     // two mirrored halves (S shape) bent by the tension
  Hold,       // keeps the previous value, jumps at the point
  Stairs,     // steps; the tension sets how many
  Smooth,     // cosine ease in/out
  Pulse,      // square wave between the two values; the tension sets the rate
  Wave,       // sine wave between the two values; the tension sets the rate
  // J-5: the remaining curve families of FL's point menu (stored mode bytes 7, 11, 8, 12, 9, 4).
  Curve2,        // single curve, exponential family
  Curve3,        // single curve, rational (sharp knee) family
  SCurve2,       // two mirrored halves of Curve2
  SCurve3,       // two mirrored halves of Curve3
  HalfSine,      // quarter-sine ease; the tension morphs ease-in ↔ half cosine ↔ ease-out
  SmoothStairs,  // stairs with eased risers; the tension sets how many
  Count
};

struct AutoPoint {
  Tick pos = 0;          // ticks from the start of the clip content
  float value = 0.5f;    // 0..1 within the clip's range
  float tension = 0.0f;  // -1..1, shapes the segment that ends here
  AutoShape shape = AutoShape::Curve;
  float frac = 0.0f;     // J-159: 0..1 of a tick after pos (recorded performances keep their sample)
  double at() const { return static_cast<double>(pos) + static_cast<double>(frac); }
  bool operator==(const AutoPoint& o) const {
    return pos == o.pos && value == o.value && tension == o.tension && shape == o.shape && frac == o.frac;
  }
};

// Pattern automation: one parameter's movement stored inside a pattern. It
// plays whenever the pattern plays (pattern mode, playlist clips, live
// launches), relative to the pattern start. Point values are the parameter's
// normalized value (0..1); recorded control changes use AutoShape::Hold.
struct PatternAutomation {
  ObjectId owner = kNoId;  // target ParamId (owner, index)
  uint32_t index = 0;
  std::vector<AutoPoint> points;  // pattern ticks, sorted by pos
  bool operator==(const PatternAutomation& o) const { return owner == o.owner && index == o.index && points == o.points; }
};

// Limits of Pattern::bars / stepsPerBar a project file accepts (Project.cpp
// rejects others). Longer note content still plays: the content length follows the notes.
inline constexpr int kMaxPatternBars = 1024;
inline constexpr int kMaxStepsPerBar = 192;
// Per-step parameters of a step-sequencer row (graph / keyboard editor, J-11).
// Defaults are neutral: a step plays C5 like before.
inline constexpr int kStepShiftMax = 100;  // shift: percent of a step
struct StepProps {
  int pitch = -1;                       // MIDI note, -1 = the default (C5)
  int pan = 0;                          // -64..64
  int release = kNoteReleaseDefault;    // 0..127
  int fine = 0;                         // cents -1200..1200
  int modX = kNoteModCentre, modY = kNoteModCentre;  // 0..255
  int shift = 0;                        // 0..kStepShiftMax: the step starts this share of a step later
  bool isDefault() const { return *this == StepProps{}; }
  bool operator==(const StepProps& o) const {
    return pitch == o.pitch && pan == o.pan && release == o.release && fine == o.fine && modX == o.modX && modY == o.modY &&
           shift == o.shift;
  }
};

// A pattern: per-channel step rows + per-channel piano-roll notes (keyed by Channel::id).
struct Pattern {
  ObjectId id = kNoId;
  std::string name = "Pattern";
  Rgb color = 0x6E9F4B;
  int stepsPerBar = 16;                            // step-sequencer resolution
  int bars = 1;
  std::map<ObjectId, std::vector<uint8_t>> steps;  // channel -> velocities per step (0 = off)
  std::map<ObjectId, std::vector<Note>> notes;     // channel -> piano-roll notes
  std::vector<PatternAutomation> automation;       // parameter lanes (at most one per target)
  // J-11: per-step parameters (parallel to `steps`; missing / short rows = defaults).
  std::map<ObjectId, std::vector<StepProps>> stepProps;
  // J-31: per-channel loop length in steps (missing / 0 = the pattern length):
  // the channel's first `n` steps (and the notes that start in them) repeat
  // through the pattern.
  std::map<ObjectId, int> stepLoop;
  // Q-1: loop mode (FL's pattern loop mode, FLP event 26): a playlist clip
  // longer than the pattern repeats it (core/ClipLoop.h). Patterns made in
  // Gridline loop; FL imports keep FL's setting (off unless set there).
  bool loop = true;

  bool operator==(const Pattern& o) const {
    return id == o.id && name == o.name && color == o.color && stepsPerBar == o.stepsPerBar && bars == o.bars &&
           steps == o.steps && notes == o.notes && automation == o.automation && stepProps == o.stepProps &&
           stepLoop == o.stepLoop && loop == o.loop;
  }
};

// --- live mode: launching playlist clips by hand ----------------------------
// Clips that start before Song::live.areaEnd form the live area: they do not
// play with the song but are launched one at a time per playlist track (from
// the playlist or the live pads), each starting on the track's launch grid.

// When a launch takes effect.
enum class LaunchGrid : uint8_t {
  Now = 0,     // immediately
  Step,        // next 1/4 beat
  HalfBeat,    // next 1/2 beat
  Beat,        // next beat
  TwoBeats,    // next 2 beats
  FourBeats,   // next 4 beats
  ClipLength,  // next multiple of the launched clip's length
  Count
};
// What a pad press does.
enum class PadMode : uint8_t {
  Trigger = 0,  // press starts the clip (again, from its start)
  Gate,         // plays while held, stops on release
  Toggle,       // press starts, the next press stops
  Count
};
// What happens when a launched clip reaches its end.
enum class FollowMode : uint8_t {
  Loop = 0,   // plays again
  Once,       // stops
  Next,       // the next clip of the track (after the last: the first)
  NextHold,   // the next clip; the last one loops
  NextStop,   // the next clip; stops after the last one
  Random,     // any clip of the track
  Shuffle,    // any other clip of the track
  Count
};

// PL-08: what a playlist track is for. Audio tracks are linked to a mixer
// track (its input, monitoring and record arm are the track's; recordings of
// that mixer track land on this track). Instrument tracks are linked to a
// rack channel (routed to its own mixer track); they hold pattern clips only.
enum class TrackMode : uint8_t { Unassigned = 0, Audio, Instrument, Count };

struct PlaylistTrack {
  std::string name;  // empty = "Track N"
  Rgb color = 0x50565E;
  bool muted = false;
  float height = 1.0f;  // display height relative to the playlist's track height (arrangement)
  // Live mode (see LaunchGrid / PadMode / FollowMode).
  LaunchGrid launch = LaunchGrid::FourBeats;
  PadMode pad = PadMode::Trigger;
  FollowMode follow = FollowMode::Loop;
  LaunchGrid syncStart = LaunchGrid::Now;  // != Now: a launched clip starts in phase with the song position on this grid
  // Folding: a track can hold the tracks below it as a group.
  int groupSize = 0;      // number of following tracks in this track's group (0 = not a group)
  bool folded = false;    // group collapsed in the playlist
  bool locked = false;    // J-11: clips on the track cannot be edited
  int flIcon = 0;         // J-39 (FL interop): FL's icon number (0 = none), kept for export
  // PL-08: track mode and its links (kNoId = none; ids that no longer exist count as none).
  TrackMode mode = TrackMode::Unassigned;
  ObjectId mixer = kNoId;    // Audio: the linked mixer track (Instrument: optional, else the channel's)
  ObjectId channel = kNoId;  // Instrument: the linked rack channel
  bool operator==(const PlaylistTrack& o) const {
    return name == o.name && color == o.color && muted == o.muted && height == o.height && launch == o.launch &&
           pad == o.pad && follow == o.follow && syncStart == o.syncStart && groupSize == o.groupSize && folded == o.folded &&
           locked == o.locked && flIcon == o.flIcon && mode == o.mode && mixer == o.mixer && channel == o.channel;
  }
  bool liveDefaults() const {
    return launch == LaunchGrid::FourBeats && pad == PadMode::Trigger && follow == FollowMode::Loop && syncStart == LaunchGrid::Now;
  }
};

struct LiveSettings {
  Tick areaEnd = 0;  // clips starting before this tick are launched by hand (0 = no live area)
  bool operator==(const LiveSettings& o) const { return areaEnd == o.areaEnd; }
};

// --- arrangement: automation clips and song markers ---------------------------

// An automation curve driving one parameter (ParamId owner + index). Placed on
// the playlist by Clip{kind = Automation, source = id}; every placement shares
// the points. Clip value v drives the parameter's normalized value
// min + v * (max - min).
struct AutomationClip {
  ObjectId id = kNoId;
  std::string name = "Automation";
  Rgb color = 0x7A8696;
  ObjectId owner = kNoId;  // target ParamId (kSongId for tempo)
  uint32_t index = 0;
  float min = 0.0f, max = 1.0f;
  std::vector<AutoPoint> points;  // sorted by pos
  Tick loop = 0;  // J-5: > 0 = the content repeats every `loop` ticks (placements longer than one cycle)
  // J-41: point shapes follow FL's exact laws (core/FlCurves). Clips of
  // projects saved before J-41 keep the earlier Gridline curves (false).
  bool flCurves = true;
  bool operator==(const AutomationClip& o) const {
    return id == o.id && name == o.name && color == o.color && owner == o.owner && index == o.index && min == o.min &&
           max == o.max && points == o.points && loop == o.loop && flCurves == o.flCurves;
  }
};

// What song playback does when it reaches a marker (J-11). The audio engine
// runs them at the exact sample (J-85; song mode, realtime playback only).
enum class MarkerAction : uint8_t {
  None = 0,
  Loop,      // FL "marker loop": playback repeats from this marker to the next one
  Skip,      // playback jumps to the next marker
  Pause,     // playback pauses here
  Stop,      // playback stops (the position returns to the start)
  SongLoop,  // J-85, FL "loop": at the song end playback continues here
  Count
};

// A song position marker, shown in the playlist ruler. A marker with a time
// signature marks a meter change from its position on.
struct Marker {
  Tick pos = 0;
  std::string name;
  int timeSigNum = 0, timeSigDen = 0;  // 0 = plain marker
  MarkerAction action = MarkerAction::None;
  // J-31 key marker: the song is in this key from here on (keyRoot 0..11,
  // keyScale = tools::scales() index); keyRoot < 0 = not a key marker.
  int keyRoot = -1, keyScale = 0;
  bool isKey() const { return keyRoot >= 0; }
  bool operator==(const Marker& o) const {
    return pos == o.pos && name == o.name && timeSigNum == o.timeSigNum && timeSigDen == o.timeSigDen && action == o.action &&
           keyRoot == o.keyRoot && keyScale == o.keyScale;
  }
};

// J-31: a named set of patterns (the pattern list can show one group).
struct PatternGroup {
  std::string name;
  std::vector<ObjectId> patterns;
  bool operator==(const PatternGroup& o) const { return name == o.name && patterns == o.patterns; }
};

// One playlist arrangement: its clips, tracks, markers and live area. A song
// has one or more; the current one is the one being edited and played.
struct Arrangement {
  std::string name = "Arrangement";
  std::vector<Clip> playlist;
  std::vector<PlaylistTrack> tracks;
  std::vector<Marker> markers;
  LiveSettings live;
  bool operator==(const Arrangement& o) const {
    return name == o.name && playlist == o.playlist && tracks == o.tracks && markers == o.markers && live == o.live;
  }
};

struct EffectSlot {
  PluginState plugin;  // empty type = free slot
  bool enabled = true;
  float mix = 1.0f;    // dry/wet 0..1
  bool operator==(const EffectSlot& o) const { return plugin == o.plugin && enabled == o.enabled && mix == o.mix; }
};

struct Send {
  ObjectId to = kNoId;  // destination MixerTrack::id
  float level = 1.0f;   // linear 0..1.25
  // Sidechain-only route: the audio feeds the destination's sidechain input
  // (read by its effects) instead of being mixed into it.
  bool sidechain = false;
  // Pre-fader send: taps the track after its effects and strip EQ, before
  // its volume and pan (default: post-fader).
  bool preFader = false;
  bool operator==(const Send& o) const {
    return to == o.to && level == o.level && sidechain == o.sidechain && preFader == o.preFader;
  }
};

inline constexpr int kEffectSlots = 10;
// Mixer strip docking (MixerTrack::dock): left (with the master), middle, right.
enum class MixerDock : int { Left = -1, Middle = 0, Right = 1 };
inline constexpr int kMaxManualLatency = 1 << 18;  // samples
// Where a mixer track records from (J-31): its input before the effects, or
// its output after the effects (and the fader with PostFader).
enum class RecordPoint : uint8_t { Input = 0, PostEffects, PostFader, Count };
// When a mixer track's input is heard through the track (J-8). Auto: while the
// track is armed for recording, and whenever the transport is stopped.
enum class InputMonitor : uint8_t { Auto = 0, On, Off, Count };
inline constexpr int kMaxInputChannels = 16;  // device input channels the engine takes
// Strip EQ defaults (MixerTrack::eqFreq / eqQ).
// (FL's strip EQ defaults: 5777 / 33145 / 55825 on its frequency table.)
inline constexpr float kStripEqDefFreq[3] = {84.12763f, 1562.2952f, 9444.748f};
inline constexpr float kStripEqDefQ = 0.7f;

struct MixerTrack {
  ObjectId id = kNoId;
  std::string name;  // empty = "Insert N" / "Master"
  Rgb color = 0x50565E;
  float volume = 0.8f;      // linear 0..1.25   (MixerParam::Volume)
  float pan = 0.0f;         // -1..1            (MixerParam::Pan)
  float stereoSep = 0.0f;   // -1 (mono) .. 1 (wide)
  bool muted = false;
  bool solo = false;
  bool invertPolarity = false;
  bool swapLR = false;
  std::vector<EffectSlot> slots = std::vector<EffectSlot>(kEffectSlots);
  std::vector<Send> sends;  // inserts send to the master by default
  // Built-in 3-band strip EQ (after the effect slots, before the fader):
  // band 0 low shelf, 1 peak, 2 high shelf. Gain 0 dB = band off.
  std::array<float, 3> eqGain = {0.0f, 0.0f, 0.0f};          // dB, -18..18
  std::array<float, 3> eqFreq = {kStripEqDefFreq[0], kStripEqDefFreq[1], kStripEqDefFreq[2]};  // Hz, 20..20000
  std::array<float, 3> eqQ = {kStripEqDefQ, kStripEqDefQ, kStripEqDefQ};                     // 0.1..10 (shelf slope / peak Q)
  bool eqActive() const { return eqGain[0] != 0.0f || eqGain[1] != 0.0f || eqGain[2] != 0.0f; }
  // --- J-12 mixer parity ---
  int icon = 0;               // in-house track icon (0 = none, see app/TrackIcons.h)
  bool disabled = false;      // bypassed: input passes straight through (no effects, EQ or strip), no CPU
  int dock = 0;               // MixerDock: where the strip sits in the mixer
  int latencyOffset = 0;      // manual latency in samples, added to the effects' latency for compensation
  bool threaded = true;       // may run on a worker thread (off: always on the audio thread)
  std::string group;          // mixer track group ("" = none)
  // Audio input (J-8): the device input channel feeding this track (0-based,
  // -1 = none); stereo takes `input` and `input + 1`. Input monitoring (see
  // InputMonitor) and the disk-record arm.
  int input = -1;
  bool inputStereo = true;
  uint8_t monitor = 0;  // InputMonitor
  bool armed = false;
  // J-31: where disk recording taps the track (RecordPoint).
  uint8_t recPoint = 0;
  // J-31 alt solo: soloed on its own; tracks feeding it stay silent (a plain
  // solo keeps the tracks routed into it audible).
  bool soloAlone = false;
  // J-39 (FL interop): a separator gap before the strip, and a locked strip
  // (FL keeps it from being edited by mouse); both are display state.
  bool separator = false;
  bool locked = false;
  int flIcon = 0;  // FL's icon number (0 = none), kept for export (`icon` is Gridline's own)
  bool hasInput() const { return input >= 0; }
  // J-31: an armed track records (its input, or its output after the effects).
  bool records() const { return armed && (input >= 0 || recPoint != 0); }

  bool operator==(const MixerTrack& o) const {
    return id == o.id && name == o.name && color == o.color && volume == o.volume && pan == o.pan &&
           stereoSep == o.stereoSep && muted == o.muted && solo == o.solo && invertPolarity == o.invertPolarity &&
           swapLR == o.swapLR && slots == o.slots && sends == o.sends && eqGain == o.eqGain && eqFreq == o.eqFreq &&
           eqQ == o.eqQ && icon == o.icon && disabled == o.disabled && dock == o.dock &&
           latencyOffset == o.latencyOffset && threaded == o.threaded && group == o.group && input == o.input &&
           inputStereo == o.inputStereo && monitor == o.monitor && armed == o.armed && recPoint == o.recPoint &&
           soloAlone == o.soloAlone && separator == o.separator && locked == o.locked && flIcon == o.flIcon;
  }
};

// Internal controller link: an output of a controller plugin (peak follower,
// X-Y pad, formula, envelope, …) drives any parameter. Evaluated on the audio
// thread every control block (64 samples); the source output (0..1) goes
// through the mapping curve, then the normalized range [min, max] of the target.
enum class ModCurve : int {
  Linear = 0, Inverted, Log, InvLog, Switch, Steps, FirstHalf, LastHalf, DeadZone, Formula, Count
};
inline constexpr int kMaxControllerOutputs = 16;

// MIDI remote control link ("link to controller"): a hardware control (CC,
// note/button, pitch bend, aftertouch, program change) drives a parameter or
// a global action (transport, record, …).
enum class RemoteSource : uint8_t { CC = 0, Note, PitchBend, ChannelPressure, KeyPressure, Program, Count };
// How a link takes over a parameter whose value differs from the hardware position.
enum class RemoteTakeover : uint8_t {
  Jump = 0,  // the parameter jumps to the control's position
  Pickup,    // nothing happens until the control passes the current value
  Scale,     // moves proportionally until both meet at an end of the range
  Count
};
// Absolute controls or relative (endless) encoders.
enum class RemoteEncoder : uint8_t {
  Absolute = 0,
  TwosComplement,  // 1..63 up, 127..65 down (127 = -1)
  BinaryOffset,    // 64 = no change, 65 = +1, 63 = -1
  SignBit,         // bit 6 is the sign: 1..63 up, 65..127 down (65 = -1)
  Count
};
// Note / button behaviour.
enum class RemoteButton : uint8_t {
  Value = 0,  // note velocity / CC value sets the parameter
  Momentary,  // maximum while held, minimum on release
  Toggle,     // each press flips between minimum and maximum
  Increment,  // each press steps up, wrapping to the minimum
  Count
};
// Global actions a link can trigger instead of a parameter.
enum class RemoteAction : uint8_t {
  None = 0, PlayPause, Stop, Record, SongMode, Metronome, CountIn, StepRecord, LoopRecord,
  NextPattern, PrevPattern, Undo, Redo, TapTempo, Count
};

struct ControllerLink {
  int cc = 1;            // controller / note / program number 0..127
  int midiChannel = -1;  // 0..15, or -1 = any
  ObjectId owner = kNoId;
  uint32_t index = 0;    // ParamId of the target (owner, index)
  float min = 0.0f, max = 1.0f;  // normalized range mapped from the control's 0..1
  // --- remote control settings (J-17); defaults = plain absolute CC link ---
  RemoteSource source = RemoteSource::CC;
  int port = -1;                       // input port number, -1 = any device
  ModCurve curve = ModCurve::Linear;   // mapping curve (ModCurve::Formula uses `formula`)
  std::string formula;                 // maps `input` (0..1) to 0..1
  bool invert = false;
  RemoteTakeover takeover = RemoteTakeover::Jump;
  RemoteEncoder encoder = RemoteEncoder::Absolute;
  float sensitivity = 1.0f;            // relative encoders: 1/128 of the range per detent × this
  RemoteButton button = RemoteButton::Value;
  RemoteAction action = RemoteAction::None;  // != None: triggers the action, (owner, index) unused
  bool operator==(const ControllerLink& o) const {
    return cc == o.cc && midiChannel == o.midiChannel && owner == o.owner && index == o.index && min == o.min &&
           max == o.max && source == o.source && port == o.port && curve == o.curve && formula == o.formula &&
           invert == o.invert && takeover == o.takeover && encoder == o.encoder && sensitivity == o.sensitivity &&
           button == o.button && action == o.action;
  }
};

struct ModLink {
  ObjectId sourceOwner = kNoId;  // channel (instrument controller) or mixer track
  int sourceSlot = -1;           // -1 = the channel's instrument; 0..9 = mixer slot
  int sourceOutput = 0;          // controller output index
  ObjectId owner = kNoId;        // target ParamId (owner, index)
  uint32_t index = 0;
  float min = 0.0f, max = 1.0f;  // normalized target range
  ModCurve curve = ModCurve::Linear;
  std::string formula;           // ModCurve::Formula: maps `input` (0..1) to 0..1
  float smoothMs = 0.0f;         // smoothing time (0 = none)
  bool enabled = true;
  bool operator==(const ModLink& o) const {
    return sourceOwner == o.sourceOwner && sourceSlot == o.sourceSlot && sourceOutput == o.sourceOutput &&
           owner == o.owner && index == o.index && min == o.min && max == o.max && curve == o.curve &&
           formula == o.formula && smoothMs == o.smoothMs && enabled == o.enabled;
  }
};

// Song information shown in the project info window (title, author, …).
struct SongInfo {
  std::string title = "Untitled";
  std::string author;
  std::string genre;
  std::string url;
  std::string comments;
  int64_t created = 0;    // creation time, Unix seconds (0 = unknown)
  int64_t workTime = 0;   // time spent editing the project, seconds

  bool operator==(const SongInfo& o) const {
    return title == o.title && author == o.author && genre == o.genre && url == o.url &&
           comments == o.comments && created == o.created && workTime == o.workTime;
  }
};

// FL Studio data kept from an imported .flp for the FL exporter (J-34): the
// original events of imported objects (plugin states, channel / insert /
// pattern / playlist settings Gridline does not model or models only
// approximately), keyed by the Gridline object they belong to ("ch/12",
// "plug/12/-1", "mx/5", "clip/40", …; see core/flp/FlpOrigin.h). The exporter
// writes an object's original data back where it still decodes to the
// object's current values, so an unchanged import exports 1:1. Empty for
// songs that were not imported from FL. Not used by playback or editing.
struct FlOrigin {
  std::map<std::string, std::string> blobs;  // key → FLP-encoded event list (binary)
  bool empty() const { return blobs.empty(); }
  bool operator==(const FlOrigin& o) const { return blobs == o.blobs; }
};

inline constexpr int kDefaultInsertTracks = 16;
inline constexpr int kDefaultPlaylistTracks = 32;
inline constexpr int kMaxPlaylistTracks = 500;   // reference: 500 [O]
inline constexpr int kMaxMixerInserts = 500;     // reference: 500 inserts + master [O]
inline constexpr int kMaxPatterns = 999;         // reference: 999 [O]

struct Song {
  SongInfo info;
  int ppq = kDefaultPpq;   // ticks per quarter note (CORE-01)
  double tempo = 140.0;    // BPM               (SongParam::Tempo)
  int timeSigNum = 4;
  int timeSigDen = 4;
  float swing = 0.0f;      // 0..1 global swing (SongParam::Swing)
  // J-138: song swing also moves piano-roll notes (false: steps only, as in projects imported from
  // .flp files, whose source app swings steps alone).
  bool swingNotes = true;
  float masterPitch = 0;   // semitones          (SongParam::MasterPitch)
  ObjectId nextId = kFirstObjectId;  // next unused object ID
  std::vector<Channel> channels;     // rack order
  std::vector<Pattern> patterns;     // pattern list order
  std::vector<Clip> playlist;        // arrangement of pattern clips
  std::vector<PlaylistTrack> tracks = std::vector<PlaylistTrack>(kDefaultPlaylistTracks);
  std::vector<MixerTrack> mixer;     // mixer[0] is the master
  std::vector<ControllerLink> links; // MIDI controller links
  std::vector<ModLink> modLinks;     // internal controller links
  std::vector<AutomationClip> automations;  // automation clip contents (arrangement)
  std::vector<Marker> markers;              // song markers, sorted by position (arrangement)
  std::vector<std::string> channelGroups;  // channel rack display groups (Channel::group indexes these)
  std::vector<PatternGroup> patternGroups;  // J-31: pattern groups (pattern list filter)
  // --- arrangements and live mode ---
  // The current arrangement's content is `playlist`, `tracks`, `markers` and
  // `live`. `arrangements` lists every arrangement in order (empty = just the
  // current one, named "Arrangement"); its entry at `currentArrangement` only
  // carries the name, the others keep their content (core/Arrangements.h).
  LiveSettings live;
  std::vector<Arrangement> arrangements;
  int currentArrangement = 0;
  // J-38: overlapping audio clips on a track crossfade automatically (project
  // setting; core/Fades.h).
  bool autoCrossfades = true;
  FlOrigin flOrigin;  // FL Studio import data for the FL exporter (see FlOrigin)

  // J-111 (collaboration): object ids are partitioned between collaborators so
  // two computers never hand out the same id: with idStride > 1 every id this
  // song allocates is congruent to idSlot modulo idStride. Session state, not
  // saved and not compared (operator== ignores it).
  uint32_t idSlot = 0, idStride = 1;
  // The id allocateId() would return next (nextId itself when not partitioned).
  ObjectId peekId() const {
    if (idStride <= 1) return nextId;
    const ObjectId r = nextId % idStride;
    return r <= idSlot ? nextId + (idSlot - r) : nextId + (idStride - r) + idSlot;
  }
  ObjectId allocateId() {
    const ObjectId id = peekId();
    nextId = id + 1;
    return id;
  }

  Tick barTicks() const {
    return static_cast<Tick>(ppq) * 4 * timeSigNum / timeSigDen;
  }
  Tick beatTicks() const { return static_cast<Tick>(ppq) * 4 / timeSigDen; }
  Tick patternLength(const Pattern& p) const { return static_cast<Tick>(p.bars) * barTicks(); }
  Tick ticksPerStep(const Pattern& p) const {
    return p.stepsPerBar > 0 ? barTicks() / p.stepsPerBar : 0;
  }
  // Content length of a pattern: its bar length, extended to cover its notes.
  Tick patternContentLength(const Pattern& p) const;
  Tick clipLength(const Clip& c) const;  // explicit length, or the pattern's / automation's content length
  Tick lengthTicks() const;              // end of last clip (0 if empty)

  // Lookup by ID (nullptr / -1 when absent).
  Channel* channel(ObjectId id);
  const Channel* channel(ObjectId id) const;
  Pattern* pattern(ObjectId id);
  const Pattern* pattern(ObjectId id) const;
  Clip* clip(ObjectId id);
  const Clip* clip(ObjectId id) const;
  MixerTrack* mixerTrack(ObjectId id);
  const MixerTrack* mixerTrack(ObjectId id) const;
  AutomationClip* automation(ObjectId id);
  const AutomationClip* automation(ObjectId id) const;
  int channelIndex(ObjectId id) const;
  int patternIndex(ObjectId id) const;
  int clipIndex(ObjectId id) const;
  int mixerIndex(ObjectId id) const;
  ObjectId masterId() const { return mixer.empty() ? kNoId : mixer[0].id; }

  bool operator==(const Song& o) const {
    return info == o.info && ppq == o.ppq && tempo == o.tempo && timeSigNum == o.timeSigNum &&
           timeSigDen == o.timeSigDen && swing == o.swing && swingNotes == o.swingNotes && masterPitch == o.masterPitch && nextId == o.nextId &&
           channels == o.channels && patterns == o.patterns && playlist == o.playlist && tracks == o.tracks &&
           mixer == o.mixer && links == o.links && modLinks == o.modLinks && automations == o.automations &&
           markers == o.markers && channelGroups == o.channelGroups && patternGroups == o.patternGroups && live == o.live &&
           arrangements == o.arrangements &&
           currentArrangement == o.currentArrangement && flOrigin == o.flOrigin;
  }
  bool operator!=(const Song& o) const { return !(*this == o); }
};

// Adds the master and `inserts` insert tracks (each sending to the master) if
// the mixer is empty.
void ensureMixer(Song& song, int inserts = kDefaultInsertTracks);
// A ready-to-use empty project: mixer, one pattern, default playlist tracks.
Song makeEmptySong();
// True if routing `from` → `to` would not create a cycle.
bool canRoute(const Song& song, ObjectId from, ObjectId to);
// Mixer tracks in processing order (sources before destinations; master last).
std::vector<ObjectId> mixerOrder(const Song& song);

// J-45: FL's route numbering for plugin side outputs and inputs (Fruity Send,
// band splitter sends, difference outputs, sidechain inputs). A plugin stores
// a number n that indexes its hosting track's route list, resolved whenever
// the routing is read (so presets follow the track they are loaded on):
//   outputs(T) = [T, destinations of T ascending]  (normal and sidechain sends)
//   inputs(T)  = [T, tracks routed to T ascending]
// Mixer indices. Output n (n >= 1) is outputs[min(n, size - 1)]; input n is
// inputs[n] (none past the end). -1 = none (n <= 0, or the track itself).
std::vector<int> flRouteOutputs(const Song& song, int mixerIndex);
std::vector<int> flRouteInputs(const Song& song, int mixerIndex);
int flRouteOutput(const Song& song, int mixerIndex, int n);
int flRouteInput(const Song& song, int mixerIndex, int n);

// Changes the project resolution, rescaling every tick position and length
// (CORE-01). Returns the number of values that did not land exactly on the
// new grid (rounded), so callers can warn before reducing PPQ.
int changePpq(Song& song, int newPpq);
// Same count without changing anything.
int countPpqRoundings(const Song& song, int newPpq);

// Internal controller links (see ModLink).
// The link driving `target` (nullptr if none). At most one link per target.
const ModLink* findModLink(const Song& song, ObjectId owner, uint32_t index);
// True if the source (owner, slot) holds a plugin that has controller outputs.
bool isControllerSource(const Song& song, ObjectId owner, int slot);
// Removes links whose source or target no longer exists; returns how many.
int pruneModLinks(Song& song);
// Maps a controller value (0..1) through a link's curve (not its range or
// formula; ModCurve::Formula returns x unchanged here).
double applyModCurve(ModCurve c, double x);
const char* modCurveName(ModCurve c);

// Display names.
std::string mixerTrackName(const Song& song, size_t index);
std::string playlistTrackName(const Song& song, size_t index);

}  // namespace gl

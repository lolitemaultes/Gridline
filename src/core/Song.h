// Gridline — song model: channels, patterns, playlist, mixer.
#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>
#include "CollabChat.h"
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

// K-169: how one child of a layer plays the layer's notes: a key zone and a velocity zone, each
// with fade widths at its ends (a note in a fade plays softer: its velocity is scaled down), a
// transpose for the layer's notes only and a solo inside the layer. Stored on the layer, keyed by
// the child's id; a child without an entry plays everything (the whole keyboard, all velocities).
// The child's own key range (Channel::keyLo/keyHi) still applies on top.
struct LayerZone {
  ObjectId child = kNoId;
  int keyLo = 0, keyHi = 127;        // inclusive keys
  int keyFadeLo = 0, keyFadeHi = 0;  // keys of fade-in above keyLo / fade-out below keyHi
  int velLo = 0, velHi = 127;        // inclusive velocities (MIDI scale)
  int velFadeLo = 0, velFadeHi = 0;
  int transpose = 0;                 // semitones, -48..48, the layer's notes only
  bool solo = false;                 // soloed inside the layer (other children go quiet)
  bool full() const {
    return keyLo <= 0 && keyHi >= 127 && keyFadeLo == 0 && keyFadeHi == 0 && velLo <= 0 && velHi >= 127 &&
           velFadeLo == 0 && velFadeHi == 0 && transpose == 0 && !solo;
  }
  bool operator==(const LayerZone& o) const {
    return child == o.child && keyLo == o.keyLo && keyHi == o.keyHi && keyFadeLo == o.keyFadeLo &&
           keyFadeHi == o.keyFadeHi && velLo == o.velLo && velHi == o.velHi && velFadeLo == o.velFadeLo &&
           velFadeHi == o.velFadeHi && transpose == o.transpose && solo == o.solo;
  }
};
// How loud a zone plays a note (0 = not at all, 1 = fully): the key fade times the velocity fade.
// `vel127` is the velocity on the MIDI scale. Pure, allocation-free (the audio thread uses it).
inline float layerZoneWeight(const LayerZone& z, int key, int vel127) {
  auto ramp = [](int x, int lo, int hi, int fadeLo, int fadeHi) -> float {
    if (x < lo || x > hi) return 0.0f;
    float w = 1.0f;
    if (fadeLo > 0 && x < lo + fadeLo) w = std::min(w, static_cast<float>(x - lo + 1) / static_cast<float>(fadeLo + 1));
    if (fadeHi > 0 && x > hi - fadeHi) w = std::min(w, static_cast<float>(hi - x + 1) / static_cast<float>(fadeHi + 1));
    return w;
  };
  return ramp(key, z.keyLo, z.keyHi, z.keyFadeLo, z.keyFadeHi) * ramp(vel127, z.velLo, z.velHi, z.velFadeLo, z.velFadeHi);
}
// K-169: the layer's macro knobs. Each drives parameters of the layer's children through internal
// controller links (ModLink with sourceSlot kLayerMacroSlot, sourceOutput = the macro).
// K-245: every channel and every mixer track has these eight macro knobs (the channel window's Misc
// page, the effect windows of a mixer track); a right-click on any plugin parameter links it to one.
inline constexpr int kLayerMacros = 8;
inline constexpr int kLayerMacroSlot = -2;
struct LayerMacro {
  std::string name;   // empty = "Macro N"
  float value = 0;    // 0..1
  bool operator==(const LayerMacro& o) const { return name == o.name && value == o.value; }
};
// K-169: layer modes (Channel::layerMode).
enum LayerMode : int { kLayerAll = 0, kLayerRandom = 1, kLayerCycle = 2, kLayerRandomNoRepeat = 3, kLayerModeCount = 4 };

// K-87: a frozen channel or mixer track. Its sound up to its live strip was rendered once to a
// file (core/Freeze.h); its plugins are unloaded while the record stands and the file plays
// instead. The unit's own state (instrument, effects, notes) is never changed by a freeze, so
// clearing the record (unfreeze) brings the plugins back exactly as they were.
struct FreezeRecord {
  // The render as a plugin state ("gl.sampler", data = the file): every walk over a project's
  // files (zipped projects, collected data files, missing files, collaboration) finds it.
  PluginState render;
  std::string fingerprint;  // what was rendered (freeze::fingerprint); another value = out of date
  int sampleRate = 0;       // the file's rate
  int arrangement = 0;      // the arrangement it was rendered for
  double seconds = 0;       // the file's length
  int latency = 0;          // mixer tracks: their effects' latency (samples at sampleRate), kept live
  bool frozen() const { return !render.data.empty(); }
  bool operator==(const FreezeRecord& o) const {
    return render == o.render && fingerprint == o.fingerprint && sampleRate == o.sampleRate && arrangement == o.arrangement &&
           seconds == o.seconds && latency == o.latency;
  }
};

// K-413: persistent channel-rack automation lanes carried across every pattern.
// Stored on Channel::automationLanes for channel-owned parameters and on
// Song::rackAutoLanes for song / mixer targets so switching patterns keeps the
// lane list visible and immediately drawable without re-mapping.
struct ChannelAutoLane {
  uint32_t index = 0;
  std::string name;
  Rgb color = 0x7A8696;
  bool operator==(const ChannelAutoLane& o) const {
    return index == o.index && name == o.name && color == o.color;
  }
};
struct SongAutoLane {
  ObjectId owner = kNoId;
  uint32_t index = 0;
  std::string name;
  Rgb color = 0x7A8696;
  bool operator==(const SongAutoLane& o) const {
    return owner == o.owner && index == o.index && name == o.name && color == o.color;
  }
};

// A channel-rack channel: an instrument plugin plus its strip and routing.
struct Channel {
  ObjectId id = kNoId;
  std::string name = "Channel";
  Rgb color = 0x5A8FD8;
  PluginState plugin;           // generator type + parameters (+ sample path)
  float gain = 0.78f;           // linear 0..1.25   (ChannelParam::Volume)
  float pan = 0.0f;             // -1..1            (ChannelParam::Pan)
  bool muted = false;           //                  (ChannelParam::Mute)
  // Channel pitch in cents, -2400..2400 (ChannelParam::Pitch; the knob in the channel strip).
  // Projects before 0.7.7 stored whole semitones ("pitch"); they load as value * 100.
  float pitchCents = 0;
  // Semitones a full MIDI pitch bend means to this channel's instrument (1..48, default 2, the
  // usual synth default). Hosted plugins without per-note tuning follow the channel pitch and the
  // note bend lines through MIDI pitch bend scaled by it: the plugin's own bend range should match.
  int bendRange = 2;
  bool sustainPedal = false;    // the sustain pedal is down (ChannelParam::Sustain; automation moves it)
  ObjectId mixerTrack = kNoId;  // MixerTrack::id; kNoId = master
  // Note processors run on this channel's notes before the instrument
  // (arpeggiator, chords, key mapping, …), in order. PluginKind::NoteFx types.
  std::vector<PluginState> noteFx;
  // --- Channel note settings (defaults keep the previous behaviour) ---
  int transpose = 0;         // K-411: per-channel transpose, whole semitones -48..48 (ChannelParam::Transpose)
  int fine = 0;              // fine tune, cents -200..200 (added to `pitchCents` + `transpose`)
  float swingMix = 1.0f;     // share of the song swing applied to this channel's steps, 0..1
  float timeShift = 0;       // every note starts this many beats later, 0..1
  int polyphony = 0;         // maximum sounding notes (0 = unlimited); the oldest is cut
  bool mono = false;         // one note at a time (a new note replaces the sounding one)
  bool porta = false;        // notes glide from the previous note's pitch
  float portaTime = 0.1f;    // glide time for porta / mono legato, seconds 0..2
  ExprSettings expr;         // K-144: what per-note expression (MPE) does on this channel
  int cutGroup = 0;          // starting a note cuts channels whose cutBy equals this (0 = none)
  int cutBy = 0;             // this channel is cut by notes of channels in this group (0 = none)
  ChannelEcho echo;
  // Layer: notes of this channel also play on these channels (the layer's own
  // instrument, if any, plays too). layerMode 0 = all children, 1 = one random child per note.
  std::vector<ObjectId> layer;
  int layerMode = 0;  // LayerMode (2 = cycle through the children, 3 = random without repeats)
  // K-169: per-child zones (see LayerZone; only children that differ from a full zone) and the
  // layer's macro knobs.
  std::vector<LayerZone> layerZones;
  std::array<LayerMacro, kLayerMacros> macros{};
  const LayerZone* layerZone(ObjectId child) const {
    for (const auto& z : layerZones)
      if (z.child == child) return &z;
    return nullptr;
  }
  // True when the layer uses anything beyond the plain all / random children of older versions.
  bool layerRack() const {
    if (layerMode >= kLayerCycle) return true;
    for (const auto& z : layerZones)
      if (!z.full()) return true;
    return false;
  }
  int group = -1;            // display group (index into Song::channelGroups), -1 = none
  // Multi-output instruments (hosted plugins): mixer track of each extra
  // output (output 2, 3, …); kNoId or missing = that output is not used.
  std::vector<ObjectId> auxOuts;
  // MIDI ports (hosted plugins): notes / MIDI a plugin sends on its output
  // port play on every channel whose input port matches (arpeggiators,
  // chord generators). -1 = none; 0..255.
  // MIDI Out channels (gl.midiout) send on their Port parameter instead (channelMidiOutPort).
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
  // J-39 (FL interop): FL's icon number for the channel (0 = none), kept for
  // export. J-211: `icon` is the Gridline icon (core/TrackIconSet.h) showing it.
  int flIcon = 0;
  int icon = 0;
  // K-87: frozen (see FreezeRecord), and hibernating: the plugins unloaded without a render,
  // woken when needed. Hibernate is this computer's choice (collaboration keeps it local).
  FreezeRecord freeze;
  bool hibernate = false;
  // K-277: the instrument keeps running while silent (Rest silent instruments leaves it alone):
  // for plugins that make sound on their own.
  bool keepRunning = false;
  // GitHub #58: hidden from the channel rack (it still plays; the rack menu shows it again). Saved
  // with the song, so everyone in a collaboration sees the same rack.
  bool rackHidden = false;
  // K-413: automation lanes persisted across patterns for this channel's parameters.
  std::vector<ChannelAutoLane> automationLanes;
  // K-128: per-track notes shown in the Notes panel's Track tab (omitted from JSON when empty).
  std::string notes;
  bool fullKeyRange() const { return keyLo <= 0 && keyHi >= 127; }

  bool isLayer() const { return !layer.empty(); }
  bool operator==(const Channel& o) const {
    return id == o.id && name == o.name && color == o.color && plugin == o.plugin && gain == o.gain &&
           pan == o.pan && muted == o.muted && pitchCents == o.pitchCents && bendRange == o.bendRange && sustainPedal == o.sustainPedal && mixerTrack == o.mixerTrack && noteFx == o.noteFx &&
           transpose == o.transpose && fine == o.fine && swingMix == o.swingMix && timeShift == o.timeShift && polyphony == o.polyphony &&
           mono == o.mono && porta == o.porta && portaTime == o.portaTime && expr == o.expr && cutGroup == o.cutGroup &&
           cutBy == o.cutBy && echo == o.echo && layer == o.layer && layerMode == o.layerMode && layerZones == o.layerZones &&
           macros == o.macros && group == o.group &&
           auxOuts == o.auxOuts && midiOutPort == o.midiOutPort && midiInPort == o.midiInPort && keyLo == o.keyLo &&
           keyHi == o.keyHi && swingTruncate == o.swingTruncate && audioClip == o.audioClip && flIcon == o.flIcon && icon == o.icon &&
           freeze == o.freeze && hibernate == o.hibernate && keepRunning == o.keepRunning && rackHidden == o.rackHidden &&
           automationLanes == o.automationLanes && notes == o.notes;
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
  // J-163: what the float value misses of an exact value (value + fine; 0 for ordinary points):
  // a recorded deck position keeps its exact place on the record. Edits work on `value`.
  double fine = 0.0;
  double at() const { return static_cast<double>(pos) + static_cast<double>(frac); }
  double exact() const { return static_cast<double>(value) + fine; }
  bool operator==(const AutoPoint& o) const {
    return pos == o.pos && value == o.value && tension == o.tension && shape == o.shape && frac == o.frac && fine == o.fine;
  }
};

// An automation curve driving one parameter (ParamId owner + index). Placed on
// the playlist by Clip{kind = Automation, source = id}; every placement shares
// the points. Clip value v drives the parameter's normalized value
// min + v * (max - min). J-173: the same clip also lives inside a pattern
// (Pattern::automation, see PatternAutomation below).
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
  // J-210: recorded events (FL's pattern events): every point steps the parameter
  // on its exact sample, and before the first point of each pass the parameter
  // keeps whatever value it has (nothing is sent), as FL plays its event data.
  bool events = false;
  bool operator==(const AutomationClip& o) const {
    return id == o.id && name == o.name && color == o.color && owner == o.owner && index == o.index && min == o.min &&
           max == o.max && points == o.points && loop == o.loop && flCurves == o.flCurves && events == o.events;
  }
};

// J-173: pattern automation clips. A pattern holds automation clips of the
// same kind as the playlist's: they play whenever the pattern plays (pattern
// mode, playlist clips, live launches), their point positions are pattern
// ticks from the pattern start. Inside a pattern a clip is identified by its
// target (at most one clip per target and pattern); `id` stays kNoId, and an
// empty `name` shows the target's name. Projects saved before J-173 hold
// plain lanes: they load as unnamed clips with range 0..1 and the earlier
// Gridline curves (flCurves false), so they play exactly as before.
using PatternAutomation = AutomationClip;

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

// K-255: a channel's lane in a pattern (core/Lanes.h). Its grid (steps per bar; 0 = the pattern's
// stepsPerBar) and what its step loop (Pattern::stepLoop, the lane length) does when the pattern
// starts again in a longer clip: restart with it (0.7.7's step loop) or run on through the clip.
enum class LaneMode : uint8_t { Restart = 0, RunOn = 1 };
inline constexpr int kMaxLaneGrid = 64;  // steps per bar a lane may have of its own
struct PatternLane {
  int grid = 0;
  LaneMode mode = LaneMode::Restart;
  bool isDefault() const { return grid == 0 && mode == LaneMode::Restart; }
  bool operator==(const PatternLane& o) const { return grid == o.grid && mode == o.mode; }
  bool operator!=(const PatternLane& o) const { return !(*this == o); }
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
  std::vector<PatternAutomation> automation;       // J-173: automation clips (at most one per target)
  // J-11: per-step parameters (parallel to `steps`; missing / short rows = defaults).
  std::map<ObjectId, std::vector<StepProps>> stepProps;
  // J-31: per-channel loop length in steps (missing / 0 = the pattern length):
  // the channel's first `n` steps (and the notes that start in them) repeat
  // through the pattern.
  std::map<ObjectId, int> stepLoop;
  // K-255: per-channel lane grid and mode (missing = the pattern's grid, restarting); only
  // channels whose lane differs from the default have an entry.
  std::map<ObjectId, PatternLane> lanes;
  // Q-1: loop mode (FL's pattern loop mode, FLP event 26): a playlist clip
  // longer than the pattern repeats it (core/ClipLoop.h). Patterns made in
  // Gridline loop; FL imports keep FL's setting (off unless set there).
  bool loop = true;
  // K-179: the pattern's own meter (0/0 = follows the song's meter at the start). Its length is
  // `bars` of its own bars, a fixed number of ticks wherever its clips sit, so meter changes in the
  // song never move or resize a clip.
  int meterNum = 0, meterDen = 0;
  bool ownMeter() const { return meterNum > 0 && meterDen > 0; }
  // 0.7.8: a linked pattern (an instrument's "Send to project", Seltmann): its notes for linkChannel
  // follow the instrument's sequence while they are what the link last wrote (linkHash, a hash of
  // those notes); once the notes are edited by hand the pattern stops following until relinked.
  // linkCycles: how many times the sequence repeats in it. kNoId = not linked (every older pattern).
  ObjectId linkChannel = kNoId;
  uint64_t linkHash = 0;
  int linkCycles = 1;

  bool operator==(const Pattern& o) const {
    return id == o.id && name == o.name && color == o.color && stepsPerBar == o.stepsPerBar && bars == o.bars &&
           steps == o.steps && notes == o.notes && automation == o.automation && stepProps == o.stepProps &&
           stepLoop == o.stepLoop && lanes == o.lanes && loop == o.loop && meterNum == o.meterNum && meterDen == o.meterDen &&
           linkChannel == o.linkChannel && linkHash == o.linkHash && linkCycles == o.linkCycles;
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
  Bar,         // grid-1118: next bar line (of the meter in force)
  TwoBars,     // next 2 bars
  FourBars,    // next 4 bars
  EightBars,   // next 8 bars
  // grid2-1118: the finer and the triplet grids (Clips' per-clip launch quantisation, Cmd+6 .. 0)
  ThirtySecond,  // next 1/32 (an eighth of a beat)
  SixteenthT,    // next 1/16 triplet (a sixth of a beat)
  EighthT,       // next 1/8 triplet (a third of a beat)
  QuarterT,      // next 1/4 triplet (two thirds of a beat)
  HalfT,         // next 1/2 triplet (four thirds of a beat)
  Count
};
// grid2-1118: the grids before the finer ones (Studio's menus list these).
inline constexpr int kLaunchGridsBasic = static_cast<int>(LaunchGrid::EightBars) + 1;
// What a pad press does.
enum class PadMode : uint8_t {
  Trigger = 0,  // press starts the clip (again, from its start)
  Gate,         // plays while held, stops on release
  Toggle,       // press starts, the next press stops
  Repeat,       // grid-1118: starts again on every launch grid line while held; plays on once released
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
  bool gridFolded = false;  // K-112c (checkpoint 52, S01): the group collapsed in Clips' clip grid (each view folds on its own; K-314: each person's own)
  bool locked = false;    // J-11: clips on the track cannot be edited
  // grid2-1118: the Clips grid's scenes (indices from 0, sorted) whose empty slot on this track has no stop
  // button (Cmd+E): launching the scene leaves the track alone there. Song data; written only when set.
  std::vector<int> noStop;
  // K-123: the track shows its recorded takes as lanes under it (engine/TakeLanes.h). K-314: view state, each
  // person's own in a collaboration (as folding).
  bool takeLanes = false;
  int flIcon = 0;         // J-39 (FL interop): FL's icon number (0 = none), kept for export
  int icon = 0;           // J-211: Gridline track icon (core/TrackIconSet.h, 0 = none)
  // PL-08: track mode and its links (kNoId = none; ids that no longer exist count as none).
  TrackMode mode = TrackMode::Unassigned;
  ObjectId mixer = kNoId;    // Audio: the linked mixer track (Instrument: optional, else the channel's)
  ObjectId channel = kNoId;  // Instrument: the linked rack channel
  // K-112b (R02-01): the automation lane under the track in the Tracks layout. It is part of the
  // track, so it moves with it and is saved with the project. laneSet: a parameter was chosen
  // (laneOwner / laneIndex, a ParamId); laneShown: the lane is open (K-314: view state, each person's own in a
  // collaboration; saved with or without a chosen parameter).
  bool laneShown = false;
  bool laneSet = false;
  ObjectId laneOwner = kNoId;
  uint32_t laneIndex = 0;
  // K-112B: the track's automation mode in the Tracks layout (AutoMode): Off ignores the track's
  // automation, Read plays it, Touch writes while a control is moved, Latch writes from the
  // first move until playback stops, Write writes from the start of playback.
  uint8_t autoMode = 1;
  // K-112B (R08-07): the sound a track keeps in the Tracks layout once its last region is deleted
  // (the reference's track keeps its channel strip and automation without regions). Only a track
  // with no instrument or audio link of its own; other layouts ignore it (kNoId = none).
  ObjectId sound = kNoId;
  // K-314 B-03: a track the Clips layout shows on this computer only, for a collaborator's instrument
  // that has no track of its own (a view of that channel, not song data): never saved and never shared
  // (written as an empty track), kept here when a collaborator's change comes in, dropped when its
  // channel goes or a shared track takes its place (collab/LocalState.cpp keepSessionState).
  bool shownHere = false;
  bool operator==(const PlaylistTrack& o) const {
    return name == o.name && color == o.color && muted == o.muted && height == o.height && launch == o.launch &&
           pad == o.pad && follow == o.follow && syncStart == o.syncStart && groupSize == o.groupSize && folded == o.folded && gridFolded == o.gridFolded &&
           locked == o.locked && takeLanes == o.takeLanes && flIcon == o.flIcon && icon == o.icon && mode == o.mode && mixer == o.mixer && channel == o.channel &&
           laneShown == o.laneShown && laneSet == o.laneSet && laneOwner == o.laneOwner && laneIndex == o.laneIndex &&
           autoMode == o.autoMode && sound == o.sound && shownHere == o.shownHere && noStop == o.noStop;
  }
  bool liveDefaults() const {
    return launch == LaunchGrid::FourBeats && pad == PadMode::Trigger && follow == FollowMode::Loop && syncStart == LaunchGrid::Now;
  }
};

// Checkpoint 59 (N58-03): a Clips scene's own name and colour (scene k: the live area's k-th stretch of
// the scene length). Empty name and colour 0: the scene shows its number only.
// grid2-1118: and its own tempo and time signature (0: none; applied when the scene launches, at its launch
// point) and its follow actions (the next scene, the previous one ... after `follow.time`).
struct SceneInfo {
  std::string name;
  Rgb color = 0;
  double tempo = 0;  // BPM (0: the scene leaves the tempo alone; a name such as "Verse 120 BPM" says it too)
  int sigNum = 0, sigDen = 0;  // time signature (0: none; a name such as "Bridge 3/4" says it too)
  FollowSpec follow;
  bool empty() const { return name.empty() && color == 0 && tempo <= 0 && sigNum <= 0 && follow.isDefault(); }
  bool operator==(const SceneInfo& o) const {
    return name == o.name && color == o.color && tempo == o.tempo && sigNum == o.sigNum && sigDen == o.sigDen && follow == o.follow;
  }
};

struct LiveSettings {
  Tick areaEnd = 0;  // clips starting before this tick are launched by hand (0 = no live area)
  // N48-07: the Clips view a 0.7.7 development build saved into the project ("grid", "arrangement";
  // "" = none). Checkpoint 59: the view is the person's own (kept in their settings with the Set, as the
  // row heights are), so it is only read, once, to carry it over there; it is never written again,
  // and a Clips save is byte for byte the other layouts'. Not part of the comparison.
  std::string view;
  // Checkpoint 59 (N58-03): scene k's name and colour, by scene (song data: undo, collaboration; no
  // trailing empty entries, and none written when there are none, so older Sets save as before).
  std::vector<SceneInfo> scenes;
  // grid2-1118: a scene's length in the live area (0: four bars of the meter at the start, as before). Set
  // when a scene's time signature changes the song's meter, so the grid's rows stay where they are.
  Tick sceneLen = 0;
  bool operator==(const LiveSettings& o) const { return areaEnd == o.areaEnd && scenes == o.scenes && sceneLen == o.sceneLen; }
};

// K-112D: the Smooth layout's window state, saved with the project as its reference saves its windows.
// library: the Library pane "open" or "closed" ("" = never set from Smooth: it opens). View state: not
// part of the comparison (no undo step), written only once set.
// D5 (R11-06): controls: the Smart Controls the person mapped by hand (Parameter Mapping > Learn): on the
// strip `strip`, screen control `pos` drives the parameter (owner, index) in place of the one the pane
// picks itself. Written only when there are some. D6 (R12-02, R12-03): song data, unlike `library`: part
// of the comparison (an undo step that marks the project modified) and shared with collaborators.
struct SmartControlMap {
  ObjectId strip = kNoId;
  int pos = 0;
  ObjectId owner = kNoId;
  uint32_t index = 0;
  bool operator==(const SmartControlMap& o) const { return strip == o.strip && pos == o.pos && owner == o.owner && index == o.index; }
  bool operator!=(const SmartControlMap& o) const { return !(*this == o); }
};
struct SmoothView {
  std::string library;
  std::vector<SmartControlMap> controls;
};

// --- arrangement: automation clips and song markers ---------------------------

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
  // K-179: patterns made while the song position is in this meter section get its meter.
  bool meterForPatterns = true;
  bool isKey() const { return keyRoot >= 0; }
  bool isMeter() const { return timeSigNum > 0 && timeSigDen > 0; }
  bool operator==(const Marker& o) const {
    return pos == o.pos && name == o.name && timeSigNum == o.timeSigNum && timeSigDen == o.timeSigDen && action == o.action &&
           keyRoot == o.keyRoot && keyScale == o.keyScale && meterForPatterns == o.meterForPatterns;
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
  // 0.7.8: which effect this is while the program runs (Document::tagSlots), so its running instance
  // follows it when it moves (up or down, to another slot or track, undo and redo). Never saved, never
  // compared; 0 until the document tags it.
  uint64_t uid = 0;
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
  // Stage room send (Song::stage.room): the track's feed into the shared Room. Its tap is
  // post-fader but before the distance gain, so the room sound does not fall with distance while the
  // dry sound does; with the room's pre-delay and early reflections (engine/DepthStage.h).
  bool stageRoom = false;
  bool operator==(const Send& o) const {
    return to == o.to && level == o.level && sidechain == o.sidechain && preFader == o.preFader && stageRoom == o.stageRoom;
  }
};

// K-112c checkpoint 57 (N56-01): an aux send the Clips layout keeps aside while the track's output (Audio
// To) plays into that same return, as the reference greys that send out and keeps its level. It is not
// in MixerTrack::sends, so it never plays (every send in `sends` plays and adds up, in every layout);
// when Audio To moves on, Clips puts it back at `index` with its level. Saved in the "live" object as
// "keptSends", only when there are some; other layouts and older versions ignore it.
struct KeptSend {
  ObjectId mixer = kNoId;  // the sending mixer track
  ObjectId to = kNoId;     // the return (the track's output now)
  float level = 1.0f;
  bool preFader = false;
  int index = 0;  // its place in the track's sends, to put it back where it was
  bool operator==(const KeptSend& o) const {
    return mixer == o.mixer && to == o.to && level == o.level && preFader == o.preFader && index == o.index;
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
inline constexpr float kInputTrimMin = -24.0f, kInputTrimMax = 24.0f;  // K-123: MixerTrack::inputTrim (dB)
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
  // Stage distance (MixerParam::Depth): 0 = the front row (no processing at all) .. 1 = the back of
  // the stage (quieter, softer highs, a little narrower; engine/DepthStage.h).
  float depth = 0.0f;
  // Stage focus (MixerParam::Focus): -1 soft (a gentle presence dip, more room) .. 0 neutral (no
  // processing at all) .. +1 present (a presence lift, drier).
  float focus = 0.0f;
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
  int icon = 0;               // in-house track icon (0 = none, see core/TrackIconSet.h)
  bool disabled = false;      // bypassed: input passes straight through (no effects, EQ or strip), no CPU
  int dock = 0;               // MixerDock: where the strip sits in the mixer
  int latencyOffset = 0;      // manual latency in samples, added to the effects' latency for compensation
  bool threaded = true;       // may run on a worker thread (off: always on the audio thread)
  std::string group;          // mixer track group ("" = none)
  // K-238: a linked group: volume, pan, mute and solo of one member move every member (set on all
  // members alike, mixops::setGroupLink).
  bool groupLink = false;
  // GitHub #58: how the mixer views show the strip (arrangement state, saved and shared; the track
  // plays and routes the same either way). `hidden`: no strip at all (the toolbar's "N hidden" brings
  // it back); `narrow`: a slim strip (name, meter, mute and solo); `groupFolded`: the track's group
  // shows as one strip, its members' own strips hidden (set on every member alike, as groupLink).
  bool hidden = false;
  bool narrow = false;
  bool groupFolded = false;
  // Audio input (J-8): the device input channel feeding this track (0-based,
  // -1 = none); stereo takes `input` and `input + 1`. Input monitoring (see
  // InputMonitor) and the disk-record arm.
  int input = -1;
  bool inputStereo = true;
  uint8_t monitor = 0;  // InputMonitor
  bool armed = false;
  // K-123: record safe: the track can't be armed (a finished take is protected). Song state,
  // shared with collaborators.
  bool recordSafe = false;
  // K-123: the input's trim (dB, kInputTrimMin..kInputTrimMax) and polarity, applied before the
  // meter, the monitoring and the recording (the track's own polarity switch acts after them).
  float inputTrim = 0.0f;
  bool inputInvert = false;
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
  // K-87: frozen / hibernating (see Channel::freeze).
  FreezeRecord freeze;
  bool hibernate = false;
  // K-224: the Stage room this track stands in (StageSettings::id; kNoId: the project's first room,
  // core/StageRoomOps.h). A track is in exactly one room.
  ObjectId room = kNoId;
  // K-245: the track's macro knobs (shown in its effect windows; see LayerMacro).
  std::array<LayerMacro, kLayerMacros> macros{};
  bool hasInput() const { return input >= 0; }
  // J-31: an armed track records (its input, or its output after the effects).
  bool records() const { return armed && (input >= 0 || recPoint != 0); }

  bool operator==(const MixerTrack& o) const {
    return id == o.id && name == o.name && color == o.color && volume == o.volume && pan == o.pan &&
           stereoSep == o.stereoSep && depth == o.depth && focus == o.focus && muted == o.muted && solo == o.solo && invertPolarity == o.invertPolarity &&
           swapLR == o.swapLR && slots == o.slots && sends == o.sends && eqGain == o.eqGain && eqFreq == o.eqFreq &&
           eqQ == o.eqQ && icon == o.icon && disabled == o.disabled && dock == o.dock &&
           latencyOffset == o.latencyOffset && threaded == o.threaded && group == o.group && groupLink == o.groupLink && hidden == o.hidden && narrow == o.narrow &&
           groupFolded == o.groupFolded && input == o.input &&
           inputStereo == o.inputStereo && monitor == o.monitor && armed == o.armed && recordSafe == o.recordSafe && recPoint == o.recPoint &&
           inputTrim == o.inputTrim && inputInvert == o.inputInvert &&
           soloAlone == o.soloAlone && separator == o.separator && locked == o.locked && flIcon == o.flIcon &&
           freeze == o.freeze && hibernate == o.hibernate && room == o.room && macros == o.macros;
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
// Osc: an Open Sound Control address (ControllerLink::oscAddress) instead of a MIDI message.
// Nrpn / Rpn: `cc` is the parameter number 0..16383 (CC 99/98 or 101/100, then data entry CC 6/38).
enum class RemoteSource : uint8_t { CC = 0, Note, PitchBend, ChannelPressure, KeyPressure, Program, Osc, Nrpn, Rpn, Count };
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
  int cc = 1;            // controller / note / program number 0..127 (NRPN / RPN: parameter 0..16383)
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
  std::string oscAddress;              // RemoteSource::Osc: the address (or address pattern) it listens to
  bool hiRes = false;                  // CC 0..31: a 14-bit control (its fine part on CC + 32)
  bool operator==(const ControllerLink& o) const {
    return cc == o.cc && midiChannel == o.midiChannel && owner == o.owner && index == o.index && min == o.min &&
           max == o.max && source == o.source && port == o.port && curve == o.curve && formula == o.formula &&
           invert == o.invert && takeover == o.takeover && encoder == o.encoder && sensitivity == o.sensitivity &&
           button == o.button && action == o.action && oscAddress == o.oscAddress && hiRes == o.hiRes;
  }
};

// K-247: the LFO board. Song-level LFOs (Song::lfos) move any parameter through ModLinks whose
// source is the LFO (sourceSlot kSongLfoSlot, sourceOwner = the LFO's id, sourceOutput 0). An LFO
// link either swings the parameter around the knob's own value (ModLink::mode kModAround: amount,
// direction) or sweeps between two values (kModAbsolute: min..max, as every other link). Any number
// of LFO links may drive one parameter (they add up); at most one other link does (findModLink).
inline constexpr int kMaxSongLfos = 32;
inline constexpr int kSongLfoSlot = -3;
inline constexpr int kLfoMaxPoints = 128;  // 0.7.9: room for a drawn stroke (was 64)
inline constexpr int kLfoColours = 8;
enum class LfoShape : uint8_t { Sine = 0, Triangle, SawUp, SawDown, Square, RandomHold, RandomGlide, Drawn, Count };
enum class LfoRestart : uint8_t { Free = 0, OnPlay, EachNote, EachBar, Count };  // (EachBar: Chaos only)
// 0.7.8: what a board source is. An LFO plays a shape; Chaos a smooth signal that never repeats (smoothed
// random walk, lfo::chaosValue); Random holds a random value until the next trigger (lfo::randomSample).
// All three move knobs through the same links (kSongLfoSlot), live on the same board and share the limit.
enum class LfoKind : uint8_t { Lfo = 0, Chaos, Random, Count };
// Random: what draws the next value. Tempo: every rate note (locked to the song while it plays); Hz:
// its own clock; Note: each note of a channel; Roll: only the Roll button (a song edit, so it syncs).
enum class RandomTrigger : uint8_t { Tempo = 0, Hz, Note, Roll, Count };
// 0.7.9: how the line after a drawn point runs to the next one.
inline constexpr uint8_t kLfoPointCurve = 0;   // straight, bent by `curve`
inline constexpr uint8_t kLfoPointSmooth = 1;  // an S curve (flat at both points: a sine between two peaks)
inline constexpr uint8_t kLfoPointStep = 2;    // holds the point's value until the next point
struct LfoPoint {
  float x = 0, y = 0, curve = 0;  // x 0..1, y -1..1, curve -1..1 (the line after the point)
  uint8_t kind = kLfoPointCurve;
  bool operator==(const LfoPoint& o) const { return x == o.x && y == o.y && curve == o.curve && kind == o.kind; }
};
struct SongLfo {
  ObjectId id = kNoId;
  std::string name;            // empty = "LFO n" (its place on the board)
  int colour = 0;              // 0..7, a board colour (theme::role::kLfo1 + colour)
  bool on = true;              // off: every target keeps its own value
  LfoShape shape = LfoShape::Sine;
  float pulseWidth = 0.5f;     // Square: the part of the cycle spent high
  int randomSteps = 4;         // Random hold / glide: values per cycle (1, 2, 4, 8, 16)
  uint32_t seed = 1;           // Random hold / glide
  std::vector<LfoPoint> points;  // Drawn (at most kLfoMaxPoints; empty = a gentle hump)
  bool drawnSteps = false;     // Drawn: flat steps between the points
  bool tempoSync = true;       // the rate in note values (rateNote, rateKind), else rateHz
  int rateNote = 5;            // index into lfo::kRateNames (1/4)
  int rateKind = 0;            // 0 straight, 1 dotted, 2 triplet
  float rateHz = 1.0f;         // 0.01..40
  float phase = 0;             // 0..360 degrees: where a cycle starts
  float depth = 1;             // 0..1: scales every target's swing
  float offset = 0;            // -1..1: shifts the output
  float smoothMs = 0;          // 0..500: rounds off corners
  float fadeIn = 0;            // 0..8 bars (seconds when not tempo synced): the swing grows after a restart
  bool unipolar = false;       // Up only: output 0..1 (the knob is the bottom of the swing)
  LfoRestart restart = LfoRestart::OnPlay;
  ObjectId noteChannel = kNoId;  // EachNote: the channel whose notes restart it
  // 0.7.8: Chaos and Random sources (kind). The seed, rate, depth, offset, smoothing (Random: the
  // glide) and Up only above apply to them too.
  LfoKind kind = LfoKind::Lfo;
  float character = 0.5f;      // Chaos: 0 calm (one slow curve) .. 1 wild (faster layers, jumps)
  RandomTrigger trigger = RandomTrigger::Tempo;  // Random
  bool centred = false;        // Random: values gather round the middle (else spread evenly)
  int quantise = 0;            // Random: 0 off, else 2..16 levels
  uint32_t roll = 0;           // Random: the Roll presses so far (Roll trigger: the value shown)
  bool operator==(const SongLfo& o) const {
    return id == o.id && name == o.name && colour == o.colour && on == o.on && shape == o.shape && pulseWidth == o.pulseWidth &&
           randomSteps == o.randomSteps && seed == o.seed && points == o.points && drawnSteps == o.drawnSteps && tempoSync == o.tempoSync &&
           rateNote == o.rateNote && rateKind == o.rateKind && rateHz == o.rateHz && phase == o.phase && depth == o.depth &&
           offset == o.offset && smoothMs == o.smoothMs && fadeIn == o.fadeIn && unipolar == o.unipolar && restart == o.restart &&
           noteChannel == o.noteChannel && kind == o.kind && character == o.character && trigger == o.trigger &&
           centred == o.centred && quantise == o.quantise && roll == o.roll;
  }
};
// ModLink::mode and ::direction (K-247).
inline constexpr uint8_t kModAbsolute = 0;  // the source sweeps the range min..max (every link before 0.7.8)
inline constexpr uint8_t kModAround = 1;    // the source swings the parameter around its own value by `amount`
inline constexpr uint8_t kModBothWays = 0, kModUpOnly = 1, kModDownOnly = 2;

struct ModLink {
  ObjectId sourceOwner = kNoId;  // channel (instrument controller) or mixer track; K-247: the LFO
  int sourceSlot = -1;           // -1 = the channel's instrument; 0..9 = mixer slot; kSongLfoSlot: a board LFO
  int sourceOutput = 0;          // controller output index
  ObjectId owner = kNoId;        // target ParamId (owner, index)
  uint32_t index = 0;
  float min = 0.0f, max = 1.0f;  // normalized target range
  ModCurve curve = ModCurve::Linear;
  std::string formula;           // ModCurve::Formula: maps `input` (0..1) to 0..1
  float smoothMs = 0.0f;         // smoothing time (0 = none)
  bool enabled = true;
  // K-247 (LFO links): how the source moves the target (kModAbsolute: min..max; kModAround: by
  // `amount`, -1..1 of the range, in `direction`), and the source turned over for this link.
  float amount = 0.0f;
  uint8_t mode = kModAbsolute;
  uint8_t direction = kModBothWays;
  bool invert = false;
  bool isLfo() const { return sourceSlot == kSongLfoSlot; }
  bool operator==(const ModLink& o) const {
    return sourceOwner == o.sourceOwner && sourceSlot == o.sourceSlot && sourceOutput == o.sourceOutput &&
           owner == o.owner && index == o.index && min == o.min && max == o.max && curve == o.curve &&
           formula == o.formula && smoothMs == o.smoothMs && enabled == o.enabled && amount == o.amount && mode == o.mode &&
           direction == o.direction && invert == o.invert;
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

// K-151: the song's swing shape (core/Groove.h). Song::swing is its amount: cell k of every
// cycle starts `timing[k] * swing * channel swing mix` cells late (negative: early), the time
// between cell starts stretches evenly, and notes starting in cell k get their velocity scaled
// toward `velocity[k]` by `accent`. The default shape, two pattern steps with the second
// half a step late at full swing, is the classic step swing, exactly as before.
struct GrooveShape {
  int grid = 0;                            // 0 pattern step, 1 eighth, 2 sixteenth, 3 thirty-second note
  int cycle = 2;                           // cells per repeat (2, 3, 4, 6, 8, 12, 16)
  std::vector<float> timing = {0.0f, 0.5f};  // per cell, fraction of a cell at full amount, -0.5..0.5
  std::vector<float> velocity;             // per cell, factor 0..2 (empty: none)
  bool operator==(const GrooveShape& o) const {
    return grid == o.grid && cycle == o.cycle && timing == o.timing && velocity == o.velocity;
  }
  bool operator!=(const GrooveShape& o) const { return !(*this == o); }
};
struct SongGroove {
  GrooveShape shape;
  float accent = 1.0f;  // how much of the velocity lane applies, 0..1
  std::string preset;   // name of the swing preset last applied ("": none), shown in the rack
  bool isDefault() const { return shape == GrooveShape{} && accent == 1.0f && preset.empty(); }
  bool operator==(const SongGroove& o) const { return shape == o.shape && accent == o.accent && preset == o.preset; }
  bool operator!=(const SongGroove& o) const { return !(*this == o); }
};

inline constexpr int kDefaultInsertTracks = 16;
inline constexpr int kDefaultPlaylistTracks = 32;
inline constexpr int kMaxPlaylistTracks = 500;   // reference: 500 [O]
inline constexpr int kMaxMixerInserts = 500;     // reference: 500 inserts + master [O]
inline constexpr int kMaxPatterns = 999;         // reference: 999 [O]

// How the mixer pans a track (Song::panLaw, project setting). Balance is the law of every project made
// before the Stage and stays theirs; new projects use Stage placement (engine/DepthStage.h).
enum class PanLaw : uint8_t { Balance = 0, ConstantPower = 1, Stage = 2 };

// One Stage room and its settings (K-224: a project has one or more rooms, Song::stage the first and
// Song::stageRooms the others; core/StageRoomOps.h). The room's space ("roomType") plays through an
// ordinary mixer track with a reverb, created the first time the user picks a space. With "No room"
// (the default) the tracks' distance and focus are kept but change nothing in the sound: no level
// drop, air shelf, narrowing, room send scaling, pre-delay or reflections. Pan and width work in
// every case.
struct StageSettings {
  ObjectId id = kNoId;         // K-224: the room's own id (kNoId: the first room of a one-room project)
  std::string name;            // K-224: the room's name ("" = the default name)
  Rgb color = 0;               // K-224: the room's colour (0 = the default)
  float width = 1.0f;          // K-224: the room's stage width, 0 .. 1.5 (1: as placed, engine/DepthStage.h)
  ObjectId room = kNoId;       // the room's space track (kNoId: none yet)
  float depthStrength = 0.5f;  // how far back the back of the stage sounds: 0 gentle (-6 dB) .. 0.5 natural (-9) .. 1 strong (-12)
  uint8_t roomType = 0;        // 0 no room, then the room types (core/MixerOps.h kRoomTypes, saved by name)
  bool hasRoom() const { return roomType != 0; }  // distance and focus are heard
  bool preDelay = true;        // the room's pre-delay follows distance (2 .. 20 ms)
  bool early = true;           // early reflections from the stage's walls
  bool frontQuiet = true;      // the front row sends 6 dB less into the room
  // The settings a project written before rooms could hold (the sound of the room, not its identity).
  bool isDefault() const { return room == kNoId && depthStrength == 0.5f && roomType == 0 && preDelay && early && frontQuiet && width == 1.0f; }
  // No name, colour or id of its own either (a one-room project saves exactly as before rooms).
  bool isPlain() const { return isDefault() && id == kNoId && name.empty() && color == 0; }
  bool operator==(const StageSettings& o) const {
    return id == o.id && name == o.name && color == o.color && width == o.width && room == o.room && depthStrength == o.depthStrength &&
           roomType == o.roomType && preDelay == o.preDelay && early == o.early && frontQuiet == o.frontQuiet;
  }
};

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
  SongGroove groove;       // K-151: the swing's shape and accents (core/Groove.h)
  // K-5: slide notes glide the sounding notes of every colour group (projects saved before 0.7.2
  // whose slides met notes of other colours, so they keep playing as they did). False (new
  // projects, FL imports): a slide note glides only the notes of its own colour, as in FL.
  bool slidesAnyColour = false;
  // GitHub #87: a slide note also glides the velocity of the notes it slides, from where it is to
  // the slide note's own velocity over the slide's length (fades in from a 0% note, fades out,
  // swells). False: projects saved before 0.7.9 whose slides would have changed a note's velocity,
  // so they keep playing as they did. True for new projects and FL imports.
  bool slidesVelocity = true;
  float masterPitch = 0;   // semitones          (SongParam::MasterPitch)
  ObjectId nextId = kFirstObjectId;  // next unused object ID
  std::vector<Channel> channels;     // rack order
  std::vector<Pattern> patterns;     // pattern list order
  std::vector<Clip> playlist;        // arrangement of pattern clips
  std::vector<PlaylistTrack> tracks = std::vector<PlaylistTrack>(kDefaultPlaylistTracks);
  std::vector<MixerTrack> mixer;     // mixer[0] is the master
  std::vector<ControllerLink> links; // MIDI controller links
  std::vector<ModLink> modLinks;     // internal controller links
  std::vector<SongLfo> lfos;         // K-247: the LFO board, in board order (at most kMaxSongLfos)
  std::vector<AutomationClip> automations;  // automation clip contents (arrangement)
  std::vector<SongAutoLane> rackAutoLanes;  // K-413: non-channel rack automation lanes persisted across patterns
  std::vector<Marker> markers;              // song markers, sorted by position (arrangement)
  std::vector<std::string> channelGroups;  // channel rack display groups (Channel::group indexes these)
  std::vector<PatternGroup> patternGroups;  // J-31: pattern groups (pattern list filter)
  // --- arrangements and live mode ---
  // The current arrangement's content is `playlist`, `tracks`, `markers` and
  // `live`. `arrangements` lists every arrangement in order (empty = just the
  // current one, named "Arrangement"); its entry at `currentArrangement` only
  // carries the name, the others keep their content (core/Arrangements.h).
  LiveSettings live;
  std::vector<KeptSend> keptSends;  // K-112c checkpoint 57 (N56-01): Clips' sends kept aside (song-wide, not per arrangement)
  SmoothView smooth;  // K-112D (not compared)
  std::vector<Arrangement> arrangements;
  int currentArrangement = 0;
  // K-112b (R02-04): the cycle area (the locators, a range of ticks; cycleEnd <= cycleStart: none
  // set) and whether Cycle is on. Part of the project, as the reference saves them.
  Tick cycleStart = 0, cycleEnd = 0;
  bool cycleOn = false;
  // K-112B (checkpoint 47): the Project End marker where the user put it (ticks; 0: not placed, the end
  // follows the last region). Saved only when placed, so older projects load and save as before.
  Tick projectEnd = 0;
  // J-38: overlapping audio clips on a track crossfade automatically (project
  // setting; core/Fades.h).
  bool autoCrossfades = true;
  // The Stage: the pan law and the shared room (engine/DepthStage.h).
  PanLaw panLaw = PanLaw::Balance;
  StageSettings stage;                    // the first Stage room
  std::vector<StageSettings> stageRooms;  // K-224: the other rooms, in order (core/StageRoomOps.h)
  // J-219: what the user typed in Track Receipt (song categories, shares, release
  // details, writers, ticked files; compact JSON, core/TrackReceipt.h). Empty = none.
  std::string trackReceipt;
  // K-110: how this project's Gridline-only plugins go into FL Studio exports ("render", "suite",
  // "suite+fallback"; core/flp/FlpExport.h GridlinePlugins). Empty = the app's default.
  std::string flExportGridline;
  FlOrigin flOrigin;  // FL Studio import data for the FL exporter (see FlOrigin)
  // K-297 (round 9): the plugin windows open when the project was last saved, in the order they
  // opened (slot -1: a channel's instrument; else a mixer track's effect slot). Opened again with the
  // project, as the reference does. View state: not compared (no undo step); written only when some
  // are open, so older projects and projects saved with none open read and write as before.
  struct OpenPluginWindow {
    ObjectId owner = kNoId;
    int slot = -1;
    bool operator==(const OpenPluginWindow& o) const { return owner == o.owner && slot == o.slot; }
  };
  std::vector<OpenPluginWindow> pluginWindows;
  // K-237: the person's workspaces, copied into the project when they asked for it (View > Workspaces >
  // Keep a copy in the project): layout id -> that layout's set as JSON (core/Workspaces.h). Read when the
  // project opens only for a layout where the person has no workspaces of their own. View state: not
  // compared (no undo step); written only when present, so older projects read and write as before.
  std::map<std::string, std::string> workspaceCopy;
  // K-86 part 2 (0.7.9): collaboration chat history. Persisted in project JSON only when non-empty
  // so empty / non-collab projects serialize byte-identically to 0.7.8. Synced peer-to-peer under
  // meta/chat:* without touching undo history; not compared by operator==.
  std::vector<ChatMessage> collabChat;
  // K-357: saved JACK / PipeWire port connections on this computer ("own" short port name ->
  // "peer" full port name, e.g. "In 1" -> "Tone:out_1"). Per-computer session state: not compared
  // (no undo step, never dirties the project), never synced in collaboration, and written to JSON
  // only when non-empty (so untouched/empty projects stay byte-identical).
  struct PortLink {
    std::string own;
    std::string peer;
    bool operator==(const PortLink& o) const { return own == o.own && peer == o.peer; }
    bool operator!=(const PortLink& o) const { return !(*this == o); }
  };
  std::vector<PortLink> portLinks;

  // J-111 (collaboration): object ids are partitioned between collaborators so
  // two computers never hand out the same id: with idStride > 1 every id this
  // song allocates is congruent to idSlot modulo idStride. Session state, not
  // saved and not compared (operator== ignores it).
  uint32_t idSlot = 0, idStride = 1;
  // K-224: the Stage room a new mixer track joins (the room shown on this computer; kNoId: the first).
  // Session state, not saved and not compared.
  ObjectId newTrackRoom = kNoId;
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

  // The bar and beat of the song's meter at the start (bar 1). Song time later in the song follows
  // its meter changes: use MeterMap (core/Meter.h) for grid, bar numbers and snapping there.
  Tick barTicks() const {
    return static_cast<Tick>(ppq) * 4 * timeSigNum / timeSigDen;
  }
  Tick beatTicks() const { return static_cast<Tick>(ppq) * 4 / timeSigDen; }
  // K-179: a pattern's own bar and beat (its meter, or the song's meter at the start).
  Tick patternBarTicks(const Pattern& p) const {
    return p.ownMeter() ? static_cast<Tick>(ppq) * 4 * p.meterNum / p.meterDen : barTicks();
  }
  Tick patternBeatTicks(const Pattern& p) const {
    return p.ownMeter() ? static_cast<Tick>(ppq) * 4 / p.meterDen : beatTicks();
  }
  int patternMeterNum(const Pattern& p) const { return p.ownMeter() ? p.meterNum : timeSigNum; }
  int patternMeterDen(const Pattern& p) const { return p.ownMeter() ? p.meterDen : timeSigDen; }
  Tick patternLength(const Pattern& p) const { return static_cast<Tick>(p.bars) * patternBarTicks(p); }
  Tick ticksPerStep(const Pattern& p) const {
    return p.stepsPerBar > 0 ? patternBarTicks(p) / p.stepsPerBar : 0;
  }
  // Content length of a pattern: its bar length, extended to cover its notes.
  Tick patternContentLength(const Pattern& p) const;
  Tick clipLength(const Clip& c) const;  // explicit length, or the pattern's / automation's content length
  Tick lengthTicks() const;              // end of last clip (0 if empty)

  // Lookup by ID (nullptr / -1 when absent).
  SongLfo* lfo(ObjectId id);  // K-247
  const SongLfo* lfo(ObjectId id) const;
  int lfoIndex(ObjectId id) const;
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
           timeSigDen == o.timeSigDen && swing == o.swing && swingNotes == o.swingNotes && groove == o.groove && slidesAnyColour == o.slidesAnyColour && slidesVelocity == o.slidesVelocity && masterPitch == o.masterPitch && nextId == o.nextId &&
           channels == o.channels && patterns == o.patterns && playlist == o.playlist && tracks == o.tracks &&
           mixer == o.mixer && links == o.links && modLinks == o.modLinks && lfos == o.lfos && automations == o.automations &&
           rackAutoLanes == o.rackAutoLanes &&
           markers == o.markers && channelGroups == o.channelGroups && patternGroups == o.patternGroups && live == o.live && keptSends == o.keptSends &&
           arrangements == o.arrangements && cycleStart == o.cycleStart && cycleEnd == o.cycleEnd && cycleOn == o.cycleOn && projectEnd == o.projectEnd &&
           currentArrangement == o.currentArrangement && autoCrossfades == o.autoCrossfades && panLaw == o.panLaw && stage == o.stage && stageRooms == o.stageRooms && trackReceipt == o.trackReceipt && flExportGridline == o.flExportGridline && flOrigin == o.flOrigin &&
           smooth.controls == o.smooth.controls;  // K-112D D6: Smart Control mappings (not the Library pane: view state)
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
// The link driving `target` (nullptr if none). At most one link per target. K-247: LFO links are left
// out (any number of them may move a parameter beside it): see lfoLinksOn.
const ModLink* findModLink(const Song& song, ObjectId owner, uint32_t index);
// K-247: the LFO links moving `target`, in the song's order.
std::vector<const ModLink*> lfoLinksOn(const Song& song, ObjectId owner, uint32_t index);
// K-247: the link from LFO `lfo` to `target` (nullptr: none).
const ModLink* findLfoLink(const Song& song, ObjectId lfo, ObjectId owner, uint32_t index);
// True if the source (owner, slot) holds a plugin that has controller outputs.
bool isControllerSource(const Song& song, ObjectId owner, int slot);
// Removes links whose source or target no longer exists; returns how many.
int pruneModLinks(Song& song);
// Maps a controller value (0..1) through a link's curve (not its range or
// formula; ModCurve::Formula returns x unchanged here).
double applyModCurve(ModCurve c, double x);
const char* modCurveName(ModCurve c);

// The MIDI port a channel sends on to other channels (-1 = none): a MIDI Out channel's Port
// parameter, else Channel::midiOutPort (hosted plugins' MIDI output).
int channelMidiOutPort(const Channel& c);
// For each channel (by index), the channels (indices) that play what it sends on its MIDI output
// port: those whose midiInPort is that port. A channel never listens to itself, and a link that
// would bring a channel's MIDI back to it through other channels is left out (listed in `loops`
// as (sender id, listener id)), so MIDI can never go round in a circle.
std::vector<std::vector<int>> midiPortListeners(const Song& s, std::vector<std::pair<ObjectId, ObjectId>>* loops = nullptr);

// Display names.
std::string mixerTrackName(const Song& song, size_t index);
std::string playlistTrackName(const Song& song, size_t index);
// R09-07: a mixer track as a send's destination (a bus): its name, except that an aux strip named
// "Aux N" (Smooth's New bus) is bus N, "Bus N", as the reference's send slots and menus read it.
std::string busName(const Song& song, size_t index);

}  // namespace gl

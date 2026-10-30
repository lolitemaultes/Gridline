// Gridline — compiled, immutable snapshot of a Song for the audio thread.
// The UI thread builds a Program after every edit; runtimes (instrument and
// effect DSP state) are shared between successive Programs so sound continues
// across edits. A few `mutable` members (marked "audio thread") are rewritten
// by the engine while internal controller links drive them; the UI never reads
// them.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include "../core/Automation.h"
#include "../core/Expr.h"
#include "../core/Fades.h"
#include "../core/Params.h"
#include "../core/Song.h"
#include "Effects.h"
#include "Generators.h"
#include "NoteActivity.h"
#include "NoteFx.h"
#include "Pdc.h"
#include "Sample.h"
#include "Scope.h"
#include "StripEq.h"
#include "TimeStretch.h"

namespace gl::packs::edison {
class CaptureRing;  // recording capture (packs/edison/edison.h)
}

namespace gl {

struct NoteEv {
  Tick on = 0, off = 0;
  // J-138: fractions of a tick (0 <= frac < 1) added to on / off. Swing places notes between
  // ticks, so the swing amount moves them smoothly at any PPQ; the engine schedules in samples.
  float onFrac = 0, offFrac = 0;
  float pitch = 60;
  float velocity = 0.8f;
  NoteProps props;     // pan, release velocity, mod X / Y, colour
  uint8_t flags = 0;   // kNoteSlide, kNotePorta
};
inline constexpr uint8_t kNoteSlide = 1;  // glides sounding notes instead of starting one
inline constexpr uint8_t kNotePorta = 2;  // starts at the previous note's pitch and glides

inline constexpr int kMaxPendingNotes = 512;
// Internal controller links are evaluated once per control block; while a
// program has links the engine renders in blocks aligned to this grid, so the
// result is the same for any audio buffer size.
inline constexpr int kControlBlock = 64;
inline constexpr int kAudioBuses = 8;  // named buses for bus send / return effects
inline constexpr int kEngineMaxBlock = 1024;  // largest block the engine renders at once
// PH-09: host parameters of the plugin build that can be linked to any parameter.
inline constexpr int kHostMacros = 16;
// J-131: host parameters of a single-plugin build (Gridline Suite): one per parameter of
// the plugin (index = parameter index), applied when they change.
inline constexpr int kMaxHostParams = 4096;

// Per-object CPU time (opt-in profiling, see AudioEngine::setProfiling):
// nanoseconds spent, accumulated by the audio thread / workers; the UI
// differentiates it over time.
struct CpuStat {
  std::atomic<uint64_t> ns{0};
  void add(uint64_t v) { ns.fetch_add(v, std::memory_order_relaxed); }
  uint64_t load() const { return ns.load(std::memory_order_relaxed); }
};

// Audio-thread state of one channel. Only the audio thread touches the
// non-atomic members.
struct ChannelRuntime {
  std::string type;
  std::unique_ptr<Generator> gen;
  float curL = 0, curR = 0;  // smoothed strip gains
  float fromL = 0, fromR = 0;  // gains at the start of the current control block (links)
  bool primed = false;
  struct Pending {
    uint32_t id;
    double off;         // end tick (with its fraction)
    int64_t at = 0;     // absolute start sample (cut groups spare notes that start together)
    float pitch = 60;   // played pitch without the channel offset (slides are relative to it)
  };
  Pending pending[kMaxPendingNotes];
  int pendingCount = 0;
  std::atomic<float> level{0};  // recent output peak, for the UI
  // J-186: the last block's output peak after the strip gains (overwritten every block, so any
  // number of UI readers; the channel rack's play light follows it).
  std::atomic<float> peak{0};
  std::atomic<int> lastStep{-1};
  // J-110: the instrument reported a voice sounding after its last block
  // (Generator::active(); editors' own Play buttons follow it).
  std::atomic<bool> sounding{false};
  NoteActivity keys;  // J-77: sounding keys + per-key hits (piano roll keyboard, pad flashes)
  ScopeTap tap;  // channel output (pre-fader)
  MidiOutRing midiOut;  // MIDI Out channels
  // Controller instruments: latest outputs (audio thread) + copies for the UI.
  float ctl[kMaxControllerOutputs] = {};
  std::atomic<float> ctlUi[kMaxControllerOutputs] = {};
  // Note processors: events waiting for the chain this block, and the
  // identity of the chain last run (a rebuilt chain releases held notes).
  NoteBuffer inbox;
  uintptr_t chainSig = 0;
  // Note dispatch state (slides / porta): pitch of the last started note.
  float lastPitch = -1;
  // Note ids: sequenced notes and notes made by the note processors are
  // numbered per channel, so the numbering (instruments seed randomness with
  // it) does not depend on how blocks interleave channels (J-30).
  uint32_t noteSeq = 0;
  uint32_t fxIdSeq = 0;
  // This block's output after the strip gains (the mixer pulls it).
  std::vector<float> outL = std::vector<float>(kEngineMaxBlock, 0.f), outR = std::vector<float>(kEngineMaxBlock, 0.f);
  CpuStat cpu;
};

struct NoteFxRuntime {
  std::string type;
  std::unique_ptr<NoteFx> fx;
  std::atomic<int> position{-1};  // editors: current step
};

// Smoothing state and live value of one internal controller link.
struct LinkRuntime {
  double value = -1;              // smoothed normalized target value (-1 = not started)
  uint32_t rng = 0x2545F491u;     // rand() in mapping formulas
  std::atomic<float> display{0};  // live target value in plugin units (UI)
  std::atomic<bool> active{false};
};

struct EffectRuntime {
  std::string type;
  std::unique_ptr<Effect> fx;
  std::atomic<float> meter{0};
  std::atomic<float> input{-120};
  std::atomic<float> bands[4] = {};
  ScopeTap tap;    // post-effect output
  ScopeTap inTap;  // pre-effect input (displays that monitor the input, e.g. the EQ)
  float ctl[kMaxControllerOutputs] = {};
  std::atomic<float> ctlUi[kMaxControllerOutputs] = {};
  // Send effects (mixer send, bus send): what this slot sent this block; the
  // destination pulls it. Allocated by the builder for those types only.
  std::vector<float> sendL, sendR;
  // Multi-output send effects (PluginInfo::sendParams): one buffer per output.
  std::vector<float> multiL[kMaxSendOutputs], multiR[kMaxSendOutputs];
  bool sent = false;  // (audio thread) sendL/R (and multiL/R) hold this block's audio
  CpuStat cpu;
};

struct MixerRuntime {
  float curL = 0, curR = 0;
  float fromL = 0, fromR = 0;  // gains at the start of the current control block (links)
  bool primed = false;
  std::atomic<float> peakL{0}, peakR{0};  // max |x| since the UI last read it
  std::atomic<float> rmsL{0}, rmsR{0};    // max block RMS since the UI last read it (RMS meters)
  ScopeTap tap;  // post-fader output
  StripEqState eq;  // strip EQ filter memory (audio thread)
  bool sent = false;  // (audio thread) processed and audible this block (destinations pull its output)
  CpuStat cpu;        // inputs, strip and sends (slots count separately)
  // Audio input (J-8): input level since the UI last read it, and the disk
  // recording capture (allocated on the UI thread while the track is armed).
  std::atomic<float> inPeakL{0}, inPeakR{0};
  std::shared_ptr<packs::edison::CaptureRing> rec;
  bool recActive = false;   // (audio thread) a take is being captured
  double recLastTick = -1;  // (audio thread) position of the last captured frame
};

struct ProgNoteFx {
  std::shared_ptr<NoteFxRuntime> rt;
  PluginState state;
  NoteFxAux aux;
  mutable PluginState live;  // state with linked parameters applied (audio thread)
  bool modded = false;       // a link targets this processor
};

// J-153: the realtime stretch of a clip (engine/TimeStretch voice): plays
// while the clip's render is not ready yet and while the channel pitch moves
// off the render's pitch. One continuous stream from where playback starts;
// a jump (seek, loop) restarts it, primed with the audio before. J-196: a
// new program keeps it while the clip plays the same sound (pitch, levels);
// a changed timing (speed, window, position) gives the clip a new
// ProgAudioClip::rtGen, which restarts the stream where the clip now is.
struct ClipVoice {
  stretch::Voice voice;
  uint32_t gen = 0;  // J-196: the rtGen the stream was started for (audio thread)
  const Sample* src = nullptr;  // reader state (audio thread)
  bool reverse = false;
  int64_t cursor = 0, endFrame = 0;  // next source frame (clip direction); silence from endFrame on
  int64_t next = -(int64_t{1} << 62);  // clip frame the stream plays next
  // J-170: a Rubber Band stream begun ahead of the clip's start (next == 0)
  // and primed over the blocks before it: the speed, pitch and source frame
  // it began with, and the start-up work it may do now (input frames).
  bool primed = false;
  double primeSpeed = 0, primePitch = 0, credit = 0;
  int64_t primeCursor = 0;
  static constexpr int kOut = 1024;
  float out[2][kOut] = {};
};
// UI thread (allocates). J-157: Rubber Band clips get its realtime engine.
std::shared_ptr<ClipVoice> makeClipVoice(double sampleRate, stretch::Engine engine = stretch::Engine::Stretch);

// J-196: a value the UI thread may change in a playing Program (relaxed
// atomic; the audio thread reads it once per block). Copies take the value.
template <typename T>
class Relaxed {
 public:
  Relaxed(T v = T{}) : v_(v) {}  // NOLINT: implicit, used like a plain T
  Relaxed(const Relaxed& o) : v_(o.get()) {}
  Relaxed& operator=(const Relaxed& o) {
    set(o.get());
    return *this;
  }
  Relaxed& operator=(T v) {
    set(v);
    return *this;
  }
  operator T() const { return get(); }  // NOLINT
  T get() const { return v_.load(std::memory_order_relaxed); }
  void set(T v) { v_.store(v, std::memory_order_relaxed); }

 private:
  std::atomic<T> v_;
};

// An audio clip compiled for playback (arrangement). Plain reads are
// stateless: the source position follows from the song position, so seeking
// and looping need no state. The realtime stretch (ClipVoice) restarts itself
// after a jump.
struct ProgAudioClip {
  Tick start = 0, end = 0;          // song ticks
  double startSec = 0, durSec = 0;  // song time at the clip start (tempo map), playback length
  SamplePtr sample;                 // prepared on the UI thread
  double srcStart = 0, srcEnd = 0;  // source window, seconds
  double speed = 1;                 // source seconds per playback second (not tempo-synced)
  double secPerTick = 0;            // > 0: tempo-synced, source seconds per song tick
  double pitch = 1;                 // frequency ratio (granular mode)
  bool granular = false;            // pitch independent from speed (the stretch)
  bool resampleSync = false;        // J-193: tempo-locked, resampled: pitch = speed x `pitch`
  stretch::Engine engine = stretch::Engine::Stretch;  // J-157: what keeps the pitch (the channel's time mode)
  bool reverse = false;
  ObjectId clip = kNoId;            // J-196: Clip::id
  // J-196: the clip's levels; the UI thread patches them into the playing
  // Program while only they change (ProgramBuilder::patchClipLevels).
  mutable Relaxed<float> gain = 1;
  mutable Relaxed<double> fadeIn = 0, fadeOut = 0;  // seconds of playback time
  mutable Relaxed<FadeShape> shapeIn = FadeShape::Smooth, shapeOut = FadeShape::Smooth;  // J-38: curve shapes
  mutable Relaxed<float> tensionIn = 0, tensionOut = 0;                                  // J-38 / J-43: curve tension (FL unit)
  mutable Relaxed<uint8_t> declickIn = 0, declickOut = 0;  // J-43: FL edge declicker mode (Clip::fadeInDeclick)
  bool cutEnd = false;              // the clip ends while its audio still plays (declick)
  // J-87: the clip's audio rendered once at offline quality (pitch, stretch,
  // tempo sync, resampling) at the program's sample rate: frame j plays j
  // frames after the clip's first sample. Used while ready and while the
  // channel pitch is still `hqSemis`; the realtime stretch covers the rest.
  stretch::RenderedClipPtr hq;
  double hqSemis = 0;
  bool hqNeeded = false;            // J-196: the clip wants a render (hq null: deferred or not possible)
  std::shared_ptr<ClipVoice> rt;  // J-153: the realtime stretch (null: not needed)
  uint32_t rtGen = 0;              // J-196: the timing `rt` plays (a new one restarts its stream)
  // J-145: the share of the render in the output (0 realtime ... 1 render) and
  // the frame it applies to next: switches between the two crossfade.
  mutable double rtMix = 0;
  mutable int64_t rtMixAt = -(int64_t{1} << 62);
};

// Adds a channel's audio clips to L/R for block samples [from, from + len),
// whose first sample sits `clock` samples after `anchorTick` (engine clock).
// `pitchSemis` is the channel pitch (knob, master pitch, automation): it
// transposes the clips without changing their timing.
void renderAudioClips(const std::vector<ProgAudioClip>& clips, const TempoMap& map, double sampleRate, double anchorTick,
                      int64_t clock, int from, int len, float* L, float* R, double pitchSemis = 0);

struct ProgChannel {
  ObjectId id = kNoId;
  std::vector<ProgAudioClip> audio;  // audio clips played through this channel (song mode), by start
  std::shared_ptr<ChannelRuntime> rt;
  PluginState plugin;
  ChannelAux aux;
  mutable float gainL = 0, gainR = 0;  // target strip gains (taper, pan, mute applied)
  mutable float pitchOffset = 0;       // channel pitch + master pitch (semitones)
  bool pitchMoves = false;             // J-153: automation / a link moves the channel pitch
  float volume = 0.78f, pan = 0, masterPitch = 0;  // unlinked strip values (masterPitch includes the channel fine tune)
  int pitch = 0;
  bool muted = false;
  int mixer = 0;               // index into Program::mixer
  std::vector<int> auxMixer;   // multi-output instruments: mixer index per extra output (-1 = unused)
  std::vector<NoteEv> song;    // song-mode events, sorted by `on`
  std::vector<ProgNoteFx> noteFx;  // note processors, in order
  mutable PluginState live;    // instrument state with linked parameters applied (audio thread)
  bool modded = false;         // a link targets an instrument parameter
  bool controller = false;     // instrument has control outputs
  bool stripLinked = false;    // a link drives volume, pan or mute
  mutable float modVolume = 0.78f, modPan = 0;  // (audio thread)
  mutable bool modMuted = false;
  // Note settings (see Channel).
  int polyphony = 0;
  bool mono = false, porta = false;
  float portaTime = 0.1f;
  int cutGroup = 0, cutBy = 0;
  std::vector<int> layer;  // layer children (Program channel indices), for live notes
  int layerMode = 0;
  int keyLo = 0, keyHi = 127;  // J-31: Channel key range (live notes outside it do not play)
  int midiPort = -1;           // J-165: MIDI Out channels (gl.midiout): their port, else -1
  // Channels listening on this channel's MIDI output port (hosted plugins'
  // MIDI out, see Channel::midiOutPort): Program channel indices.
  std::vector<int> midiTargets;
};


// One input a mixer node pulls (summed in list order, so the result does not
// depend on which thread produced it), with its delay compensation.
struct ProgInput {
  enum Kind : uint8_t { Channel, Send, SlotSend, Sidechain, ChannelAux, Track };  // Track: a track's output at unity (J-45)
  Kind kind = Channel;
  int src = 0;    // Channel / ChannelAux: channel index; others: source mixer index
  int index = 0;  // Send / Sidechain: index into the source's list; SlotSend: slot index; ChannelAux: extra output
  int out = -1;   // SlotSend from a multi-output effect: output (PluginInfo::sendParams index)
  PdcTap tap;
  bool fromChannel() const { return kind == Channel || kind == ChannelAux; }
};

struct ProgSlot {
  std::shared_ptr<EffectRuntime> rt;
  PluginState plugin;
  ChannelAux aux;  // pack effects (see packs::prepareAux)
  mutable bool enabled = true;  // (audio thread when linked)
  mutable float mix = 1;     // (audio thread when linked)
  mutable PluginState live;  // state with linked parameters applied (audio thread)
  bool modded = false;
  bool controller = false;   // effect has control outputs
  int sendTo = -1;           // send effects: destination mixer index
  int busSend = -1;          // audio bus written by a bus send
  int busRecv = -1;          // audio bus read by a bus return
  int multiSend[kMaxSendOutputs] = {-1, -1, -1, -1};  // PluginInfo::sendParams destinations
  // J-45 (PluginInfo::sideInParam): the slot's sidechain input is its own
  // (sideOwn) — the output of track sideSrc (-1 = none: no sidechain).
  bool sideOwn = false;
  int sideSrc = -1;
  std::vector<ProgInput> sideIn;  // the source's output, compensated to this slot
  int latency = 0;           // samples this slot adds (0 when bypassed)
  // Bus returns: the senders of the bus that run before this track, in
  // summation order, each compensated to this slot's position.
  std::vector<ProgInput> auxIn;
};

struct ProgMixer {
  ObjectId id = kNoId;
  std::shared_ptr<MixerRuntime> rt;
  mutable float gainL = 0, gainR = 0;  // (audio thread when linked)
  mutable float sep = 0;
  float volume = 0.8f, pan = 0, sepBase = 0;  // unlinked values
  bool invert = false, swap = false;
  mutable StripEqSettings eq;  // strip EQ (MixerTrack::eqGain/eqFreq/eqQ); linked bands change it (audio thread)
  bool silent = false;      // muted, or silenced by another track's solo
  bool soloSilent = false;  // silenced by another track's solo
  mutable bool muteMod = false;  // mute state set by a link (audio thread)
  bool muteLinked = false;
  bool stripLinked = false;  // a link drives volume or pan
  mutable float modVolume = 0.8f, modPan = 0;  // (audio thread)
  std::vector<ProgSlot> slots;
  mutable std::vector<std::pair<int, float>> sends;  // (mixer index, level); linked levels change (audio thread, no resize)
  std::vector<std::pair<int, float>> sidechain;  // sidechain-only sends (mixer index, level)
  std::vector<uint8_t> sendPre, sidechainPre;    // per send: taps the pre-fader signal (Send::preFader)
  bool hasPreSend = false;   // some send taps the pre-fader signal (the engine keeps a copy)
  bool disabled = false;     // MixerTrack::disabled: input passes straight through
  bool threaded = true;      // MixerTrack::threaded: off = runs on the audio thread only
  int latencyManual = 0;     // MixerTrack::latencyOffset (samples, part of latOwn)
  bool hasSidechain = false;  // some track feeds this track's sidechain input
  // Graph (see Program::graph): inputs pulled into the bus and the sidechain
  // buffer, and plugin delay compensation figures.
  std::vector<ProgInput> inputs, scInputs;
  // Audio input (MixerTrack::input): first device channel (-1 = none), pair or
  // mono, monitoring mode (InputMonitor) and the record arm.
  int inFirst = -1;
  bool inStereo = true;
  uint8_t monitor = 0;
  bool armed = false;
  uint8_t recPoint = 0;  // J-31: RecordPoint (MixerTrack::recPoint)
  int latIn = 0;    // path latency at the track input (compensated: all inputs arrive aligned)
  int latOwn = 0;   // latency of the track's own effects
  int latOut() const { return latIn + latOwn; }
};

// Compiled internal controller link (see ModLink).
struct ProgLink {
  enum Kind : uint8_t {
    ChanPlugin, ChanVolume, ChanPan, ChanPitch, ChanMute, ChanNoteFx,
    MixVolume, MixPan, MixSep, MixMute, SlotParam, SlotMix,
    MixEqGain, MixEqFreq, MixEqQ,  // param = band 0..2
    MixSend,                       // param = index into ProgMixer::sends
    SlotEnable
  };
  // source: a channel instrument (srcChannel) or a mixer slot (srcMixer, srcSlot)
  int srcChannel = -1, srcMixer = -1, srcSlot = -1, output = 0;
  Kind kind = ChanPlugin;
  int chan = -1, mixer = -1, slot = -1, fx = -1, param = -1;
  ParamDesc desc;
  double base = 0;  // unlinked normalized value (formula variable `base`)
  float min = 0, max = 1, smoothMs = 0;
  ModCurve curve = ModCurve::Linear;
  Expr formula;     // ModCurve::Formula; variables: input, base, t, beat
  std::shared_ptr<LinkRuntime> rt;
};

// PH-09: a host macro linked to a parameter (plugin build).
struct ProgMacro {
  bool valid = false;
  ProgLink target;  // target fields only
};

// Automation of one parameter by playlist automation clips (arrangement):
// evaluated every control block in song mode, applied like a controller link.
struct ProgAutoLane {
  ProgLink target;  // target fields (kind, indices, desc, rt); source fields unused
  AutomationLane lane;
};

struct ProgPattern {
  ObjectId id = kNoId;
  Tick length = 0;
  std::vector<std::vector<NoteEv>> perChannel;  // index = channel index, sorted by `on`
  std::vector<ProgAutoLane> autos;              // pattern automation (pattern mode), pattern ticks
};

// Live mode: a clip of the live area compiled for launching. Content ticks
// run from 0 (the clip's start, its offset applied) to `length`.
struct ProgLiveClip {
  ObjectId clip = kNoId;  // Clip::id
  int track = 0;
  Tick length = 1;
  struct Ev {
    int channel = 0;  // Program channel index
    NoteEv e;         // content ticks
  };
  std::vector<Ev> notes;             // sorted by e.on
  std::vector<ProgAutoLane> autos;   // pattern automation / automation clip, content ticks
  int audioChannel = -1;             // audio clip: Program channel index
  std::vector<ProgAudioClip> audio;  // audio clip compiled at tick 0 on the constant tempo map (0 or 1 entry)
};
// Live settings of one playlist track that has live clips.
struct ProgLiveTrack {
  int track = 0;
  LaunchGrid launch = LaunchGrid::FourBeats;
  PadMode pad = PadMode::Trigger;
  FollowMode follow = FollowMode::Loop;
  LaunchGrid syncStart = LaunchGrid::Now;
  std::vector<int> clips;  // indices into Program::liveClips, by playlist position
};

// Task graph of one block: channels (generators) feed mixer tracks, tracks
// feed tracks. Tasks 0..C-1 are channels, C.. are mixer tracks (by index);
// absent tasks (no instrument, track not processed) have deps < 0.
struct ProgGraph {
  int channels = 0, tasks = 0, active = 0;
  std::vector<int> deps;                     // predecessors per task (-1 = absent)
  std::vector<std::vector<int>> dependents;  // tasks unblocked when a task completes
  std::vector<int> serial;                   // single-thread order (channels by index, then mixer order)
  std::vector<uint8_t> mainOnly;             // per task: must run on the audio thread (worker 0)
  // Audio-thread scratch for task handout, sized here (UI thread).
  std::unique_ptr<std::atomic<int>[]> pending, ready;
};

// J-85: a marker action compiled for the audio thread (song mode, realtime
// playback). Playback acts when it reaches `tick` from earlier positions.
struct ProgMarkerEvent {
  enum Kind : uint8_t { Skip, Pause, Stop, LoopBack };
  double tick = 0;
  double from = -1;   // LoopBack (FL marker loop): applies only from this position on
  double target = 0;  // Skip / LoopBack: where playback continues
  Kind kind = Skip;
};

struct Program {
  double tempo = 140;
  int ppq = kDefaultPpq;
  Tick barTicks = 384;
  Tick beatTicks = 96;
  Tick songEnd = 0;      // last clip end (0 = empty)
  Tick songLoopEnd = 0;  // song end rounded up to whole bars (>= one bar)
  // J-85 marker actions: events sorted by tick, and where the song end wraps
  // to (the last song loop marker before songLoopEnd; < 0 = the song start).
  std::vector<ProgMarkerEvent> markerEvents;
  double songLoopStart = -1;
  std::vector<ProgChannel> channels;
  std::vector<ProgPattern> patterns;
  std::vector<ProgMixer> mixer;
  std::vector<int> order;  // mixer processing order (indices), master last
  int master = 0;
  ProgGraph graph;
  bool pdc = true;        // plugin delay compensation applied
  int outputLatency = 0;  // samples from the transport to the master output (PDC on); renders skip them
  std::vector<std::shared_ptr<PdcDelay>> pdcLines;  // owners of the delay lines the inputs use
  std::vector<ProgLink> links;  // internal controller links
  bool hasNoteFx = false;       // some channel has note processors
  // Arrangement: automation clips, tempo map (song mode), constant map (pattern mode).
  std::vector<ProgAutoLane> autos;
  TempoMap tempoMap, flatMap;
  // Links and automation are evaluated on a fixed control-block grid.
  bool controlGrid() const { return !links.empty() || !autos.empty() || hasPatternAutos || hasMacros || hasHostBank; }
  // PH-09: host macros (index = macro) and whether any is linked.
  std::vector<ProgMacro> macros;
  bool hasMacros = false;
  // J-131: the plugin whose parameters the host parameters drive (target fields of
  // parameter 0; ProgramBuilder::setHostParamBank) and its descriptors.
  ProgLink hostBank;
  bool hasHostBank = false;
  const PluginInfo* hostBankInfo = nullptr;
  bool hasCutGroups = false;    // some channel cuts channels (cut groups)
  bool hasPatternAutos = false;  // some pattern or live clip carries automation
  // Live mode: the live area's clips and the tracks they sit on.
  std::vector<ProgLiveClip> liveClips;
  std::vector<ProgLiveTrack> liveTracks;
  int liveTrackIndex(int track) const {
    for (size_t i = 0; i < liveTracks.size(); ++i)
      if (liveTracks[i].track == track) return static_cast<int>(i);
    return -1;
  }
  int liveClipIndex(ObjectId clip) const {
    for (size_t i = 0; i < liveClips.size(); ++i)
      if (liveClips[i].clip == clip) return static_cast<int>(i);
    return -1;
  }

  const ProgPattern* pattern(ObjectId id) const {
    for (const auto& p : patterns)
      if (p.id == id) return &p;
    return nullptr;
  }
  int channelIndex(ObjectId id) const {
    for (size_t i = 0; i < channels.size(); ++i)
      if (channels[i].id == id) return static_cast<int>(i);
    return -1;
  }
};

// Builds Programs and owns the runtime registries (UI thread only).
class ProgramBuilder {
 public:
  std::shared_ptr<const Program> build(const Song& song);
  // Sample rate the program will run at (hosted plugins prepare for it).
  void setSampleRate(double sr) { sampleRate_ = sr; }
  double sampleRate() const { return sampleRate_; }
  // Plugin delay compensation (Options): delays earlier paths so every input
  // of a mixing point arrives aligned. Off: latencies are reported only.
  void setPdc(bool on) { pdc_ = on; }
  bool pdc() const { return pdc_; }
  // J-30: builds never wait for sample decoding (SampleCache::NonBlocking):
  // samples not decoded yet are queued and left out (silence) until a later
  // build; the caller rebuilds when SampleCache::generation() changes. Off by
  // default (offline renders and tools need every sample).
  void setAsyncSamples(bool on) { asyncSamples_ = on; }
  // J-196: while an edit goes on (a knob or handle held), builds start no
  // new clip renders (pitch, stretch): renders already made still play, the
  // realtime stretch plays the rest. deferredRenders(): the last build left
  // some out (the caller builds again once the edit is over).
  void setDeferRenders(bool on) { deferRenders_ = on; }
  bool deferRenders() const { return deferRenders_; }
  bool deferredRenders() const { return deferred_; }
  // J-196: the last build (an edit in progress) left some clips without the
  // realtime voice they need (a few new ones per build): build again.
  bool voicesPending() const { return voicesPending_; }
  // J-196: writes the clip levels of `song` (gains, fades, declick; see
  // clipLevelsOnly in Document.h) into the Program this builder built last,
  // while it plays, instead of a build. Only valid when nothing else changed
  // since that build; false when it cannot (that Program is gone, a clip it
  // plays is missing): build instead.
  bool patchClipLevels(const Song& song);
  // J-196: counters (tests): builds, level patches, clip voices made / kept.
  uint64_t builds() const { return builds_; }
  uint64_t patches() const { return patches_; }
  uint64_t voicesMade() const { return voicesMade_; }
  uint64_t voicesReused() const { return voicesReused_; }
  // PH-09: parameters the host macros drive (index = macro, owner kNoId = not
  // linked); compiled into Program::macros by the next build.
  void setHostMacros(const std::vector<ParamId>& targets) { macroTargets_ = targets; }
  const std::vector<ParamId>& hostMacros() const { return macroTargets_; }
  // J-131: host parameter k drives parameter k of the plugin at `firstParam`
  // (channelPluginParam(ch, 0) / slotParam(track, slot, 0)); kNoId owner = none.
  void setHostParamBank(ParamId firstParam) { bankTarget_ = firstParam; }
  // True when a hosted plugin's reported latency differs from the one the
  // last Program was compensated for (the caller rebuilds).
  bool latencyChanged() const;
  // True when a hosted instance asks to be replaced (reload after a crash,
  // sandbox or processing options changed): the caller rebuilds.
  bool staleInstances() const;
  // Runtime access for the UI (meters). May return nullptr.
  std::shared_ptr<ChannelRuntime> channelRuntime(ObjectId id) const;
  std::shared_ptr<MixerRuntime> mixerRuntime(ObjectId id) const;
  std::shared_ptr<EffectRuntime> effectRuntime(ObjectId track, int slot) const;
  std::shared_ptr<NoteFxRuntime> noteFxRuntime(ObjectId channel, int index) const;
  // Live value (plugin units) of a parameter driven by an internal controller
  // link, as last computed by the audio thread. False when not linked/running.
  bool modulatedValue(ParamId id, double& value) const;
  // Latest output (0..1) of a controller, for editors and meters. 0 when absent.
  float controllerOutput(ObjectId owner, int slot, int output) const;

  // J-167: project loads prepare the plugins one at a time, between interface frames, in a
  // builder of their own (the playing project keeps this one), then hand them over: the
  // next build() of that song reuses them (same id and type) instead of creating them.
  struct PreloadItem {
    bool effect = false;    // a mixer slot effect, else a channel's instrument
    ObjectId owner = kNoId;  // channel / mixer track
    int slot = -1;           // effect slot (effects only)
    std::string type;        // plugin type
  };
  // Every channel instrument and mixer slot effect of `song`, in build order.
  static std::vector<PreloadItem> preloadItems(const Song& song);
  // Creates the runtime of one item and restores its state (prepareHost), as build() would.
  // False when the plugin could not be created (unknown or unavailable type).
  bool preload(const Song& song, const PreloadItem& item);
  // Takes over the runtimes `from` prepared (replacing ours with the same id); `from` is left empty.
  void adoptPreloaded(ProgramBuilder&& from);

 private:
  std::map<ObjectId, std::shared_ptr<ChannelRuntime>> channels_;
  std::map<ObjectId, std::shared_ptr<MixerRuntime>> mixers_;
  std::map<std::pair<ObjectId, int>, std::shared_ptr<EffectRuntime>> effects_;
  std::map<std::pair<ObjectId, int>, std::shared_ptr<NoteFxRuntime>> noteFx_;
  std::map<uint64_t, std::shared_ptr<LinkRuntime>> links_;  // by target ParamId
  double sampleRate_ = 44100.0;
  bool pdc_ = true;
  bool asyncSamples_ = false;
  bool deferRenders_ = false, deferred_ = false, voicesPending_ = false;
  std::weak_ptr<const Program> last_;
  uint64_t builds_ = 0, patches_ = 0, voicesMade_ = 0, voicesReused_ = 0;
  // J-196: the realtime stretch voice of each audio clip (id, live area) and
  // how it plays, kept from build to build (`same`: the same sound; `inPlace`:
  // also at the same place in it, else a new `gen` restarts the stream).
  struct ClipVoiceSlot {
    std::shared_ptr<ClipVoice> voice;
    const Sample* sample = nullptr;
    stretch::Engine engine = stretch::Engine::Stretch;
    bool reverse = false, resampleSync = false;
    Tick start = 0;
    double srcStart = 0, srcEnd = 0, speed = 0, secPerTick = 0;
    uint32_t gen = 0;
    bool same(const ClipVoiceSlot& o) const;
    bool inPlace(const ClipVoiceSlot& o) const;
  };
  std::map<std::pair<ObjectId, bool>, ClipVoiceSlot> clipVoices_;
  std::vector<ParamId> macroTargets_;
  ParamId bankTarget_;
  // Plugin delay compensation: computes latencies and fills ProgMixer::inputs
  // and the task graph (ProgramGraph.cpp).
  void compileGraph(Program& prog);
  // Delay lines by edge (destination track, kind, source, index, slot).
  using EdgeKey = std::tuple<ObjectId, int, ObjectId, int, int>;
  struct EdgeLine {
    std::shared_ptr<PdcDelay> line, old;
  };
  std::map<EdgeKey, EdgeLine> lines_;
  bool built_ = false;
  // Hosted plugins whose live latency the last build used.
  struct LiveLatency {
    std::shared_ptr<EffectRuntime> fx;
    std::shared_ptr<ChannelRuntime> gen;
    int value = 0;
  };
  std::vector<LiveLatency> liveLatency_;
};

// Note events of one pattern for one channel (steps + piano roll), in pattern
// ticks, with the channel's time shift, swing mix and echo applied, plus the
// notes of layer channels that trigger this channel.
std::vector<NoteEv> compilePatternChannel(const Song& song, const Pattern& p, ObjectId channel);
// The same for one channel's own notes only (no layer parents).
std::vector<NoteEv> compileChannelNotes(const Song& song, const Pattern& p, const Channel& c);
// J-138: song swing as a time warp of pattern tick `x` (steps and piano-roll notes alike):
// inside each pair of steps, counted from the bar line, the pair's midpoint moves
// `swing` x half a step later and the pair's ends stay put. Odd steps per bar leave
// the bar's last step straight. swing 0 returns x unchanged.
double swingWarp(double x, double swing, Tick stepTicks, int stepsPerBar);
inline double noteOnTick(const NoteEv& e) { return static_cast<double>(e.on) + e.onFrac; }
inline double noteOffTick(const NoteEv& e) { return static_cast<double>(e.off) + e.offFrac; }

// Data an instrument or effect needs besides its parameters (samples, slices,
// pack data), prepared on the UI thread. Used by the builder and plugin hosts.
void prepareChannelAux(const PluginState& state, ChannelAux& aux);

}  // namespace gl

// Gridline — realtime engine (EN-03/EN-04): transport, sample-accurate event
// dispatch, generators, mixer graph, metering. process() runs on the audio
// thread; everything else is called from the UI thread. The same code renders
// offline, so exports match playback exactly.
#pragma once
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "../core/MidiSync.h"
#include "AudioLogger.h"
#include "Program.h"
#include "WorkerPool.h"

namespace gl {

// Pattern: loops the selected pattern. Song: plays the arrangement. Live:
// the transport runs freely and plays only clips launched from the live area.
enum class PlayMode { Pattern, Song, Live };

class AudioEngine {
 public:
  static constexpr int kMaxBlock = kEngineMaxBlock;
  // Blocks shorter than this always run on the audio thread alone (the
  // hand-off would cost more than it saves).
  static constexpr int kParallelMinFrames = 32;

  explicit AudioEngine(double sampleRate = 44100.0);
  ~AudioEngine();

  // ---- UI thread ----
  void setSampleRate(double sr);  // only while the stream is stopped
  double sampleRate() const { return sr_; }
  void setProgram(std::shared_ptr<const Program> p);
  void collectGarbage();  // frees retired programs once the audio thread is done with them
  const std::shared_ptr<const Program>& program() const { return current_; }

  void play();
  // J-49 transport position rules (FL-like song position marker):
  //  - setPosition / jumpTo with seek = true (ruler clicks, scrubbing, time
  //    selections, marker jumps, MIDI song position) also set the *seek
  //    position*: where Stop returns. Automatic moves pass seek = false.
  //  - stop(): stops and returns to the seek position (0 when none was set);
  //    with setReturnToSeek(false) it returns to the start of the time
  //    selection (song mode) or 0 instead. Repeated play / stop cycles keep
  //    returning to the same seek position until a new one is set.
  //  - pause(): stops where playback is (the seek position is kept).
  //  - stopToStart(): stops, returns to 0 and clears the seek position (new
  //    project, offline renders).
  //  - a mode change (pattern / song / live) clears the seek position (the
  //    transport starts over at 0 in the new mode).
  void stop();
  void stopToStart();
  void pause();
  void togglePlay();
  void setMode(PlayMode m);
  void setPattern(ObjectId pattern);
  void setPosition(Tick tick, bool seek = true);
  void setReturnToSeek(bool on) { returnToSeek_.store(on, std::memory_order_relaxed); }
  bool returnToSeek() const { return returnToSeek_.load(std::memory_order_relaxed); }
  double seekPosition() const { return seekTick_.load(std::memory_order_relaxed); }
  // Where stop() lands now (UI thread).
  double stopTarget() const;
  void setLooping(bool loop);  // offline renders turn looping off
  // Offline render (export): hosted plugins are told (time info / process
  // mode). Set before processing starts.
  void setOffline(bool on) { offline_ = on; }
  // Offline renders (J-30): the engine renders blocks of `frames` (<= kMaxBlock)
  // on its own absolute sample grid, buffered, whatever chunk sizes the caller
  // asks for — so the output is identical for any caller block size, hosted
  // plugins included. 0 = off: host blocks are processed as given (realtime).
  // Set before processing starts.
  void setFixedBlock(int frames);
  // Fixed-block mode: no internal block crosses absolute sample `sample`
  // (frames since construction), so a command sent when the caller reaches it
  // (e.g. stop at the end of an export's body) takes effect exactly there.
  void addBlockBarrier(int64_t sample);
  // Offline render: sampler interpolation quality (RenderInterp, 0 = linear).
  // Builds the interpolation tables (call before processing starts).
  void setInterpolation(int quality);
  // Song-mode time selection: playback wraps from `end` back to `start`.
  // end <= start clears it (the song loops over its full length again).
  void setLoopRange(Tick start, Tick end);
  void noteOn(ObjectId channel, int pitch, float velocity);
  void noteOff(ObjectId channel, int pitch);
  void panic();  // all sound off
  // Live mode: launch / release (gate pads) a live-area clip, stop a track or
  // every track. Launches start on the track's launch grid; a launch while
  // stopped starts the transport.
  void launchClip(ObjectId clip);
  void releaseClip(ObjectId clip);
  void stopLiveTrack(int track);
  void stopAllLive();
  // Clip playing / waiting to start on a playlist track (kNoId = none).
  ObjectId liveActive(int track) const;
  ObjectId liveQueued(int track) const;
  // Plays a sample directly to the output (browser preview); nullptr stops it.
  void preview(SamplePtr sample);
  bool previewing() const { return previewing_.load(); }
  void setMetronome(bool on) { metronome_.store(on); }
  // MIDI input (called from the MIDI driver thread; single producer).
  void midiEvent(uint8_t status, uint8_t d1, uint8_t d2);
  // Channel that MIDI input plays (UI thread).
  void setMidiTarget(ObjectId channel) { midiTarget_.store(channel); }
  // Notes on MIDI channel `midiCh` (0..15) play `channel`; kNoId = the
  // channel set by setMidiTarget (the selected one). UI thread.
  void setMidiChannelRoute(int midiCh, ObjectId channel) {
    if (midiCh >= 0 && midiCh < 16) midiRoute_[midiCh].store(channel);
  }
  ObjectId midiChannelRoute(int midiCh) const { return midiCh >= 0 && midiCh < 16 ? midiRoute_[midiCh].load() : kNoId; }
  // Pitch bend range (semitones) applied to built-in instruments for notes from
  // MIDI channel `midiCh` (hosted plugins get the raw pitch bend).
  void setBendRange(int midiCh, float semitones) {
    if (midiCh >= 0 && midiCh < 16) bendRange_[midiCh].store(semitones);
  }
  // Pedals: CC 64 sustain, CC 66 sostenuto, CC 67 soft (velocity × factor).
  void setPedals(bool sostenuto, bool soft, float softFactor) {
    sostenutoOn_.store(sostenuto);
    softOn_.store(soft);
    softFactor_.store(softFactor);
  }
  // Starts playback after a count-in of `ticks` (metronome clicks while the
  // transport waits at its position).
  void playWithCountIn(double ticks);
  bool countingIn() const { return countingIn_.load(); }
  // MIDI sync output (clock / MTC) for the transport, sample-accurate.
  void setSyncOut(SyncOut mode) { syncMode_.store(static_cast<int>(mode)); }
  MidiEventRing& syncEvents() { return syncRing_; }
  // Absolute sample counter at the start of the last processed block, and the
  // steady-clock time (seconds) when that block was processed (MIDI out timing).
  int64_t blockSample() const { return blockSampleUi_.load(); }
  double blockTime() const { return blockTimeUi_.load(); }

  bool playing() const { return playing_.load(); }
  PlayMode mode() const { return mode_.load(); }
  ObjectId pattern() const { return pattern_.load(); }
  double position() const { return posTick_.load(); }  // current tick
  float cpuLoad() const { return cpu_.load(); }
  // Worst block load (fraction of real time) since the UI last read it.
  float takeCpuPeak() { return cpuPeak_.exchange(0.f, std::memory_order_relaxed); }

  // Processing threads for the mixer graph, including the audio thread
  // (1 = single-threaded). UI thread; starts pool threads as needed. The
  // output is bit-identical for any thread count.
  void setThreads(int n);
  int threads() const { return threads_.load(std::memory_order_relaxed); }
  // Worker threads that run with real-time scheduling (the others can be
  // preempted mid-block and stall the audio thread waiting for their tasks).
  int realtimeWorkers() const { return pool_.realtimeThreads(); }
  int workerThreads() const { return pool_.threads(); }
  // Per channel / insert / slot CPU time (CpuStat in the runtimes). Off by
  // default; costs two clock reads per object per block when on.
  void setProfiling(bool on) { profiling_.store(on, std::memory_order_relaxed); }
  bool profiling() const { return profiling_.load(std::memory_order_relaxed); }
  // Samples between the transport and the master output (plugin delay
  // compensation) for the current program.
  int outputLatency() const;
  float masterPeakL() const { return outPeakL_.load(); }
  float masterPeakR() const { return outPeakR_.load(); }

  // ---- audio input & recording (J-8) ----
  // Device input: the capture callback pushes interleaved frames (single
  // producer: the device thread); each processed block takes the same number
  // of frames from the queue (missing frames are silence). UI thread:
  // setInputChannels while the stream is stopped.
  void setInputChannels(int channels);
  int inputChannels() const { return inChannels_.load(std::memory_order_relaxed); }
  void pushInput(const float* interleaved, int frames);
  void clearInput();  // drops queued input (UI thread, stream stopped)
  // Frames kept queued as a jitter buffer between the capture and playback
  // callbacks (adds to the input latency).
  void setInputPrefill(int frames) { inPrefill_.store(std::max(0, frames)); }
  // Peak of a device input channel since the UI last read it.
  float takeInputPeak(int channel);
  int inputUnderruns() const { return inUnderruns_.load(std::memory_order_relaxed); }
  // Disk recording: armed mixer tracks capture their input while recording is
  // on and the transport plays (not during a count-in). `latency` = samples by
  // which the input lags the music the performer hears (device input + output
  // latency + manual offset); captured frames are placed that much earlier.
  void setRecording(bool on) { recordOn_.store(on); }
  bool recordingOn() const { return recordOn_.load(); }
  void setRecordLatency(int samples);
  int recordLatency() const { return recLatency_.load(std::memory_order_relaxed); }
  // Punch range (song ticks): only frames inside it are captured. end <= start: none.
  void setPunch(double start, double end) {
    punchA_.store(start);
    punchB_.store(end);
  }
  // Take boundaries, in the order the audio thread produced them. A Start
  // event comes before the take's first frame enters the track's capture
  // ring; an End event after its last frame.
  struct RecEvent {
    enum Kind : uint8_t { Start, End } kind = Start;
    ObjectId mixer = kNoId;
    size_t ringPos = 0;  // absolute frame index in the track's capture ring
    double tick = 0;     // Start: song position of the take's first frame
  };
  bool popRecEvent(RecEvent& e);

  // ---- J-31 ----
  // Song position jump that waits for the next multiple of `grid` ticks while
  // the transport plays (quantized jumps); grid <= 0 or stopped: at once.
  void jumpTo(Tick tick, Tick grid);
  bool jumpPending() const { return jumpPendingUi_.load(std::memory_order_relaxed); }
  // Incoming MIDI notes land on the scale: `mask` bit k = pitch class
  // (root + k) is in the scale; mask 0 = off. Ties snap down.
  void setMidiScaleSnap(int root, uint16_t mask) {
    midiSnap_.store(mask ? (static_cast<uint32_t>(mask & 0xFFF) | (static_cast<uint32_t>(((root % 12) + 12) % 12) << 12)) : 0u,
                    std::memory_order_relaxed);
  }
  static int snapToScaleMask(int pitch, int root, uint16_t mask);
  // Audio logger (nullptr = off). UI thread; the previous logger is freed once
  // the audio thread can no longer use it.
  void setLogger(std::shared_ptr<AudioLogger> logger);
  const std::shared_ptr<AudioLogger>& logger() const { return loggerOwner_; }

  // ---- audio thread ----
  // Writes n interleaved stereo frames.
  void processInterleaved(float* out, int frames);
  // Writes n frames to separate buffers (overwrites).
  void process(float* L, float* R, int frames);
  static constexpr int kOfflineBlock = 512;  // fixed block of offline renders

 private:
  struct Cmd {
    enum Kind { Play, Stop, Pause, Mode, Pattern, Position, NoteOn, NoteOff, Panic, Loop, Preview, LoopRange,
                Launch, Release, LiveStop, LiveStopAll, Jump } kind;
    ObjectId id = kNoId;
    int pitch = 0;
    float value = 0;
    double tick = 0, tick2 = 0;
    const Sample* sample = nullptr;
  };
  void send(const Cmd& c);
  void processDirect(float* L, float* R, int frames);  // host blocks as given
  void drain(const Program* p);
  void processBlock(const Program* p, float* L, float* R, int n);
  void dispatch(const Program* p, int from, int to);  // sequenced events for block samples [from, to)
  // Releases every sequenced note; `hard` (stop/pause) also clears latched note processors.
  void releaseAll(const Program* p, int offset, bool hard = false);
  // Note-on/off for a channel: straight to its instrument, or into its note processor chain.
  void sendOn(const ProgChannel& ch, uint32_t id, float pitch, float velocity, int offset, int color,
              const NoteProps* props = nullptr);
  void sendOff(const ProgChannel& ch, uint32_t id, int offset);
  // Hosted instruments' MIDI output → channels on the same MIDI port.
  void routePluginMidi(const Program* p, int n);
  // Live note on/off (keyboard, MIDI): also plays a layer channel's children.
  void liveOn(const Program* p, const ProgChannel& ch, uint32_t id, float pitch, float velocity, int color);
  void liveOff(const Program* p, const ProgChannel& ch, uint32_t id);
  // Sequenced note start with the channel's note settings (cut groups, mono,
  // porta, polyphony) and slide notes. `at` = absolute start sample.
  void startNote(const Program* p, const ProgChannel& ch, const NoteEv& e, int offset, int64_t at, double spt);
  void cutPending(const ProgChannel& ch, int index, int offset);  // cuts and forgets pending note `index`
  void runNoteFx(const Program* p, int n);  // note processor chains for this block
  void evaluateLinks(const Program* p, int n, double blockTick);  // internal controller links + automation
  // Live mode: clip launching, follow actions and the launched clips' notes for block samples [from, to).
  void dispatchLive(const Program* p, int from, int to);
  void liveCommand(const Program* p, const Cmd& c);
  void clearLive();
  void applyLinkValue(const Program* p, const ProgLink& l, double normalized);
  // Transport clock through the tempo map (song mode: tempo automation;
  // pattern mode: constant). `clk` samples after the tick `anchor`.
  const TempoMap& clockMap(const Program* p) const { return tMode_ == PlayMode::Song ? p->tempoMap : p->flatMap; }
  double tickAt(const Program* p, double anchor, double clk) const { return clockMap(p).tickAfterSamples(anchor, clk, sr_); }
  // First sample (counted from the anchor) at or after `tick`.
  int64_t sampleAt(const Program* p, double anchor, double tick) const {
    return static_cast<int64_t>(std::ceil(clockMap(p).samplesBetween(anchor, tick, sr_) - 1e-9));
  }
  void drainMidi(const Program* p);
  // Mixer graph of one block (AudioEngineGraph.cpp).
  void runGraph(const Program* p, int n);
  static void graphJob(void* self, int worker);
  void claimTasks(int worker);
  void runTask(int task, int worker);
  void runChannel(size_t channel, int worker);
  void runMixer(size_t mixer, int worker);
  void pullInput(const ProgInput& in, float* accL, float* accR) const;
  void readDeviceInput(int n);                        // input queue → inBuf_ (start of a block)
  // Armed tracks → capture rings: before the graph (inputs) or after it (post = tracks recording their output).
  void captureRecording(const Program* p, int n, bool post = false);
  void logAudio(const Program* p, const float* mL, const float* mR, int n);  // J-31 audio logger
  void pushRecEvent(const RecEvent& e);
  // Input block of a mixer track (mono tracks duplicate their channel).
  const float* inputL(const ProgMixer& m) const;
  const float* inputR(const ProgMixer& m) const;
  double loopEnd(const Program* p) const;
  double loopStart() const;  // where a wrap lands (time selection start, else 0)
  // Where the transport wraps now: loopEnd, or the song end while pastLoop_.
  double transportEnd(const Program* p) const;
  // Moves the transport to `tick` (sample clock 0) and updates pastLoop_.
  void landAt(double tick);

  double sr_;
  // Programs: owned on the UI thread, published to the audio thread.
  std::shared_ptr<const Program> current_;
  std::vector<std::pair<std::shared_ptr<const Program>, uint64_t>> retired_;
  SamplePtr previewOwner_;
  std::vector<std::pair<SamplePtr, uint64_t>> retiredSamples_;
  const Sample* pvSample_ = nullptr;  // audio thread
  double pvPos_ = 0;
  std::atomic<bool> previewing_{false};
  std::atomic<bool> metronome_{false};
  // MIDI queue (single producer: MIDI thread, single consumer: audio).
  struct MidiMsg {
    uint8_t s, a, b;
  };
  static constexpr size_t kMidiQueue = 1024;
  std::vector<MidiMsg> mqueue_ = std::vector<MidiMsg>(kMidiQueue);
  std::atomic<size_t> mHead_{0}, mTail_{0};
  std::atomic<ObjectId> midiTarget_{kNoId};
  ObjectId midiNoteCh_[128] = {};  // channel each sounding MIDI note was sent to
  bool sustain_ = false;
  bool sustained_[128] = {};
  std::atomic<ObjectId> midiRoute_[16] = {};
  std::atomic<float> bendRange_[16] = {};
  std::atomic<bool> sostenutoOn_{true}, softOn_{true};
  std::atomic<float> softFactor_{0.6f};
  bool sostenuto_ = false, soft_ = false;
  bool sostenutoHeld_[128] = {};  // notes caught by the sostenuto pedal
  bool sostenutoOff_[128] = {};   // their key was released while the pedal was down
  uint8_t midiNoteSrc_[128] = {}; // MIDI channel each sounding MIDI note came from
  float bend_[16] = {};           // current pitch bend per MIDI channel (semitones)
  // Count-in and sync output.
  int64_t countInLeft_ = 0, countInTotal_ = 0, countInBeat_ = 1;
  std::atomic<bool> countingIn_{false};
  std::atomic<int> syncMode_{0};
  MidiClockOut clockOut_;
  MidiEventRing syncRing_;
  std::atomic<int64_t> blockSampleUi_{0};
  std::atomic<double> blockTimeUi_{0};
  double clickPhase_ = 0, clickFreq_ = 0;
  int clickLeft_ = 0, clickStart_ = -1;
  std::atomic<const Program*> live_{nullptr};
  std::atomic<uint64_t> blocks_{0};

  // Command queue (single producer: UI, single consumer: audio).
  static constexpr size_t kQueue = 4096;
  std::vector<Cmd> queue_;
  std::atomic<size_t> qHead_{0}, qTail_{0};

  // Transport (audio thread), published through atomics.
  bool tPlaying_ = false, tLoop_ = true;
  bool offline_ = false;         // offline render (see setOffline)
  // Fixed-block mode (setFixedBlock): rendered frames not handed out yet.
  int fixed_ = 0;
  std::vector<float> fifoL_, fifoR_;
  int fifoPos_ = 0, fifoLen_ = 0;
  static constexpr int kMaxBarriers = 16;
  int64_t barriers_[kMaxBarriers] = {};
  int barrierCount_ = 0;
  int interp_ = 0;               // offline render interpolation (see setInterpolation)
  bool ctxWasPlaying_ = false;   // transport state of the previous block (ProcessContext::jumped)
  double ctxNextTick_ = 0;
  double loopA_ = 0, loopB_ = 0;  // song-mode time selection (audio thread)
  // J-40: the position was moved past the time selection (ruler click, marker
  // or quantized jump): playback plays on from there to the song end, and only
  // then wraps into the selection, instead of bouncing straight back.
  bool pastLoop_ = false;
  PlayMode tMode_ = PlayMode::Pattern;
  ObjectId tPattern_ = kNoId;
  double anchorTick_ = 0;   // tick at sample clock 0
  int64_t clock_ = 0;       // samples since the anchor
  uint64_t lastMapSig_ = 0;  // tempo map of the last block (re-anchor when it changes)
  double curTick_ = 0;       // position at the end of the last block
  double startTick_ = 0;    // where Play started (Stop returns here)
  std::atomic<bool> playing_{false};
  std::atomic<PlayMode> mode_{PlayMode::Pattern};
  std::atomic<ObjectId> pattern_{kNoId};
  std::atomic<double> posTick_{0};
  std::atomic<double> seekTick_{0};        // J-49: the song position marker (where Stop returns)
  std::atomic<bool> returnToSeek_{true};
  double uiLoopA_ = 0, uiLoopB_ = 0;       // UI-thread copy of the time selection
  std::atomic<float> cpu_{0}, outPeakL_{0}, outPeakR_{0};

  // Note processor timing: transport segments of the current block.
  struct Segment {
    int from, len;
    double anchor;
    int64_t clock;
  };
  static constexpr int kMaxSegments = 72;
  Segment segs_[kMaxSegments];
  int segCount_ = 0;
  int64_t absClock_ = 0;      // samples rendered since construction (control-block grid)
  // Note starts of the current block, in time order (dispatch).
  struct BlockStart {
    int64_t at;  // clock sample
    int ch;
    const NoteEv* ev;
  };
  static constexpr int kMaxBlockStarts = 4096;
  std::vector<BlockStart> starts_ = std::vector<BlockStart>(kMaxBlockStarts);
  NoteBuffer fxA_, fxB_;      // chain scratch

  // Live mode (audio thread): per playlist track, the clip playing and the
  // clip / stop waiting for its launch grid. Clips are kept by id so program
  // rebuilds do not disturb them.
  struct LiveTrack {
    ObjectId active = kNoId;
    Tick origin = 0;  // transport tick of the active clip's content tick 0
    ObjectId queued = kNoId;
    Tick queuedAt = 0, queuedOrigin = 0;
    bool stopQueued = false;
    Tick stopAt = 0;
  };
  LiveTrack liveTracks_[kMaxPlaylistTracks];
  std::atomic<ObjectId> liveActiveUi_[kMaxPlaylistTracks] = {};
  std::atomic<ObjectId> liveQueuedUi_[kMaxPlaylistTracks] = {};
  uint32_t liveRng_ = 0x9E3779B9u;
  // Audio of launched audio clips, rendered with the channels (this block).
  struct LivePiece {
    int clip, from, len;
    double anchor;  // content tick at the piece's clock 0
    int64_t clock;
  };
  static constexpr int kMaxLivePieces = 256;
  LivePiece livePieces_[kMaxLivePieces];
  int livePieceCount_ = 0;

  // Scratch buses (audio thread).
  std::vector<std::vector<float>> busL_, busR_;
  std::vector<std::vector<float>> scL_, scR_;    // per mixer track sidechain inputs
  std::vector<std::vector<float>> preL_, preR_;  // per mixer track pre-fader signal (tracks with pre-fader sends)
  MidiPortBus ports_;  // notes MIDI Out channels hold per port (read by MIDI-input effects)
  std::vector<float> tmpL_, tmpR_;
  // Per-thread scratch for tasks (index = worker; 0 = audio thread).
  struct Scratch {
    float chL[kEngineMaxBlock], chR[kEngineMaxBlock];
    float dryL[kEngineMaxBlock], dryR[kEngineMaxBlock];
    float auxL[kEngineMaxBlock], auxR[kEngineMaxBlock];
    float sideL[kEngineMaxBlock], sideR[kEngineMaxBlock];  // J-45 slot side inputs
  };
  std::vector<Scratch> scratch_;
  // The block the graph tasks render (set by processBlock before runGraph).
  struct BlockCtx {
    const Program* p = nullptr;
    int n = 0;
    bool grid = false;
    int gpos = 0;
    size_t nb = 0;
    bool profile = false;
    ProcessContext ctx;
  } bc_;
  WorkerPool pool_;
  std::atomic<int> threads_{1};
  std::atomic<bool> profiling_{false};
  std::atomic<float> cpuPeak_{0};
  // Audio input (J-8).
  static constexpr size_t kInputQueueFrames = size_t{1} << 15;
  std::vector<float> inQueue_;  // interleaved, kInputQueueFrames * kMaxInputChannels
  std::atomic<size_t> inHead_{0}, inTail_{0};  // frames written / read
  std::atomic<int> inChannels_{2};
  std::atomic<int> inUnderruns_{0};
  std::atomic<int> inPrefill_{0};
  bool inPrimed_ = false;  // (audio thread) the jitter buffer has filled
  std::vector<std::vector<float>> inBuf_;  // per input channel, this block
  std::vector<float> silence_;
  std::atomic<float> inPeak_[kMaxInputChannels] = {};
  std::atomic<bool> recordOn_{false};
  std::atomic<int> recLatency_{0};
  std::atomic<double> punchA_{0}, punchB_{0};
  static constexpr size_t kTickHistory = size_t{1} << 17;  // delayed timeline for latency compensation
  std::vector<double> tickHist_;
  size_t tickHistPos_ = 0;
  size_t tickHistLat_ = 0;  // latency the history was last read with (audio thread)
  std::vector<double> recTick_;  // delayed position of each frame of this block (-1 = not recording)
  static constexpr size_t kRecEvents = 1024;
  std::vector<RecEvent> recEvents_ = std::vector<RecEvent>(kRecEvents);
  std::atomic<size_t> reHead_{0}, reTail_{0};

  // J-31: quantized position jump (audio thread) and the audio logger.
  bool pendJump_ = false;
  double pendJumpAt_ = 0, pendJumpTo_ = 0;
  std::atomic<bool> jumpPendingUi_{false};
  std::atomic<uint32_t> midiSnap_{0};
  std::shared_ptr<AudioLogger> loggerOwner_;
  std::vector<std::pair<std::shared_ptr<AudioLogger>, uint64_t>> retiredLoggers_;
  std::atomic<AudioLogger*> logger_{nullptr};
  float logL_[kEngineMaxBlock] = {}, logR_[kEngineMaxBlock] = {};

  // Task hand-out (see claimTasks).
  std::atomic<int> head_{0}, readyTail_{0}, done_{0};
};

}  // namespace gl

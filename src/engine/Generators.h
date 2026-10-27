// Gridline — instrument (generator) runtime. One instance per channel, used
// only by the audio thread (or the offline renderer).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <memory>
#include <vector>
#include "../core/PluginInfo.h"
#include "Sample.h"

namespace gl {

// Notes held on numbered MIDI ports (0..255) during the current block: MIDI Out
// channels publish the notes they play on their port, effects with a MIDI
// input (the pitch corrector's MIDI / harmonize modes) read them. Owned by the
// engine, touched only by the audio thread.
struct MidiPortBus {
  static constexpr int kPorts = 256;
  // Relaxed atomics: MIDI Out channels may render on different worker threads
  // (the readers run in mixer tasks ordered after them by the task graph).
  std::array<std::array<std::atomic<uint8_t>, 128>, kPorts> vel{};  // 0 = released, else velocity 1..127
  std::array<std::atomic<bool>, kPorts> used{};
  void set(int port, int note, int velocity) {
    if (port < 0 || port >= kPorts || note < 0 || note > 127) return;
    vel[static_cast<size_t>(port)][static_cast<size_t>(note)].store(static_cast<uint8_t>(std::clamp(velocity, 0, 127)),
                                                                     std::memory_order_relaxed);
    if (velocity > 0) used[static_cast<size_t>(port)].store(true, std::memory_order_relaxed);
  }
  uint8_t get(int port, int note) const {
    if (port < 0 || port >= kPorts || note < 0 || note > 127) return 0;
    return vel[static_cast<size_t>(port)][static_cast<size_t>(note)].load(std::memory_order_relaxed);
  }
  // Start of a block (audio thread, before any task): publishers re-announce
  // what they still hold.
  void beginBlock() {
    for (size_t p = 0; p < used.size(); ++p)
      if (used[p].load(std::memory_order_relaxed)) {
        for (auto& v : vel[p]) v.store(0, std::memory_order_relaxed);
        used[p].store(false, std::memory_order_relaxed);
      }
  }
};

struct ProcessContext {
  double sampleRate = 44100.0;
  double tempo = 140.0;
  int ppq = 96;
  double tick = 0;       // song/pattern position at the start of the block
  bool playing = false;  // transport running
  MidiPortBus* ports = nullptr;  // engine's MIDI port notes (null offline / in previews)
  // Transport details for hosted plugins (time info).
  int tsNum = 4, tsDen = 4;            // time signature
  bool looping = false;                // playback wraps from loopEnd back to loopStart (ticks)
  double loopStart = 0, loopEnd = 0;
  bool recording = false;
  bool offline = false;                // offline render (export), faster than real time
  int interp = 0;                      // offline render: sampler interpolation (RenderInterp; 0 = linear)
  bool jumped = false;                 // the position jumped (play, seek, loop wrap) in this block
};

// Lock-free ring of outgoing MIDI messages (audio thread → UI thread).
struct MidiOutRing {
  struct Msg {
    uint8_t s, a, b;
  };
  static constexpr size_t kSize = 1024;
  Msg buf[kSize];
  std::atomic<size_t> head{0}, tail{0};
  void push(uint8_t s, uint8_t a, uint8_t b) {
    size_t h = head.load(std::memory_order_relaxed);
    if (h - tail.load(std::memory_order_acquire) >= kSize) return;
    buf[h % kSize] = {s, a, b};
    head.store(h + 1, std::memory_order_release);
  }
  bool pop(Msg& m) {
    size_t t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) return false;
    m = buf[t % kSize];
    tail.store(t + 1, std::memory_order_release);
    return true;
  }
};

// Pack-defined prepared data (parsed tables, compiled formulas, hosted
// plugin graphs…). Built on the UI thread, read-only on the audio thread.
struct AuxData {
  virtual ~AuxData() = default;
};

struct InsSettings;  // engine/ChannelIns.h

// Data an instrument needs besides its parameters, prepared on the UI thread.
struct ChannelAux {
  std::vector<SamplePtr> samples;  // sampler: [0]; drum pads: one per pad (may be null)
  std::vector<int64_t> slices;     // slicer: slice start frames
  std::shared_ptr<const AuxData> custom;  // pack data (see packs::prepareAux)
  // Channel instrument envelopes / filter (PluginState::ins, J-33); null = none.
  std::shared_ptr<const InsSettings> ins;
  SamplePtr sample(size_t i = 0) const { return i < samples.size() ? samples[i] : nullptr; }
};

// Per-note properties beyond pitch and velocity (piano-roll note properties).
// All values are neutral by default.
struct NoteProps {
  float pan = 0;         // -1 (left) .. 1 (right)
  float release = 0.5f;  // release velocity 0..1
  float modX = 0.5f;     // instrument-defined 0..1 (sampler: filter cutoff), 0.5 = neutral
  float modY = 0.5f;     // instrument-defined 0..1 (sampler: filter resonance), 0.5 = neutral
  int color = 0;         // colour group / MIDI channel 0..15
  bool neutral() const { return pan == 0 && release == 0.5f && modX == 0.5f && modY == 0.5f && color == 0; }
};

// Linear pitch glide helper for voices (slide notes, porta). Advance once per sample.
struct PitchGlide {
  double step = 0, to = 0;
  int64_t left = 0;
  void start(double from, double target, double seconds, double sampleRate) {
    left = std::max<int64_t>(1, static_cast<int64_t>(seconds * sampleRate));
    to = target;
    step = (target - from) / static_cast<double>(left);
  }
  bool active() const { return left > 0; }
  // Moves `pitch` one sample along the glide.
  void advance(double& pitch) {
    if (left <= 0) return;
    pitch = --left == 0 ? to : pitch + step;
  }
};

class Generator {
 public:
  virtual ~Generator() = default;

  // Events for the next process() call, at sample offsets within that block.
  // `id` identifies the note for its later noteOff. pitch is fractional MIDI.
  void noteOn(uint32_t id, double pitch, float velocity, int offset) { push(Ev::make(Ev::On, id, pitch, velocity, offset)); }
  // Note-on with per-note properties (pan, release velocity, mod X/Y, colour).
  void noteOn(uint32_t id, double pitch, float velocity, int offset, const NoteProps& props) {
    Ev e = Ev::make(Ev::On, id, pitch, velocity, offset);
    e.props = props;
    e.hasProps = !props.neutral();
    push(e);
  }
  void noteOff(uint32_t id, int offset) { push(Ev::make(Ev::Off, id, 0, 0, offset)); }
  void allNotesOff(int offset) { push(Ev::make(Ev::AllOff, 0, 0, 0, offset)); }
  // Glides the sounding note `id` to `pitch` over `seconds` (slide notes,
  // porta, mono legato). Only instruments with supportsGlide() follow it.
  void glide(uint32_t id, double pitch, double seconds, int offset) {
    Ev e = Ev::make(Ev::Glide, id, pitch, 0, offset);
    e.time = seconds;
    push(e);
  }
  // Cuts the note `id` quickly (cut groups, polyphony limit, mono) — unlike
  // noteOff this also ends one-shot voices.
  void cut(uint32_t id, int offset) { push(Ev::make(Ev::Cut, id, 0, 0, offset)); }
  virtual bool supportsGlide() const { return false; }
  // J-68: the step an instrument's own sequencer is playing (−1 = none), for
  // the editor's running-step display. Read on the UI thread: implementations
  // publish it through an atomic.
  virtual int displayStep() const { return -1; }
  // A raw MIDI channel message for the instrument (controllers, pitch bend,
  // channel / poly pressure from a MIDI keyboard). Only hosted plugins use it.
  void midi(uint8_t status, uint8_t d1, uint8_t d2, int offset) {
    Ev e = Ev::make(Ev::Midi, static_cast<uint32_t>(status) | (static_cast<uint32_t>(d1) << 8) | (static_cast<uint32_t>(d2) << 16), 0, 0, offset);
    push(e);
  }
  // Silences immediately (stop / panic).
  void kill() {
    count_ = 0;
    onKill();
  }

  // Adds n frames to L/R. `state` holds the current parameter values.
  void process(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R,
               int n);
  virtual bool active() const = 0;  // any voice sounding
  // J-112: the instrument plays on its own (its own sequence, not notes it is
  // given: Seltmann's latched pattern). Audio thread, after a block; the
  // engine publishes it as plugin playback (AudioEngine::pluginPlayback).
  virtual bool ownPlayback() const { return false; }
  // J-112: stop that playback with a normal release (Play pressed while the
  // transport is stopped). Audio thread, between blocks; no allocation.
  virtual void stopOwnPlayback() {}
  // MIDI Out channels send their notes here (set once by the program builder).
  virtual void setMidiOut(MidiOutRing*) {}
  // Hosted plugins: called on the UI thread every time a Program is built,
  // with the state the next blocks will use and the engine's sample rate
  // (loads changed state chunks, re-prepares processing). Never concurrent
  // with itself; may run while the audio thread is inside process().
  virtual void prepareHost(const PluginState& state, double sampleRate) {
    (void)state;
    (void)sampleRate;
  }
  // J-87: UI thread, Program build, before prepareHost: the channel only
  // feeds playlist audio clips (no notes in any pattern), so instruments can
  // skip per-note resources (the sampler's stretch voices).
  virtual void setClipOnly(bool clipOnly) { (void)clipOnly; }
  // J-150: live MIDI notes reach this instrument as raw messages (onMidi), not
  // as notes (DJ decks decode their controller's notes themselves).
  virtual bool rawMidi() const { return false; }
  // Processing latency in samples (for plugin delay compensation).
  virtual int latencySamples() const { return 0; }
  // Latency with `state` at `sampleRate` (UI thread, Program build; see
  // Effect::latencyFor). Built-in instruments render without latency.
  virtual int latencyFor(const PluginState& state, double sampleRate) const {
    (void)state;
    (void)sampleRate;
    return latencySamples();
  }
  // UI thread, Program build: true when this instance must be replaced by a
  // fresh one of the same type (hosted plugin reload, sandbox switch).
  virtual bool stale() const { return false; }
  // Multi-output instruments: extra stereo outputs, valid for the frames of
  // the last process() call (the engine routes them to mixer tracks).
  virtual int auxOutputCount() const { return 0; }
  virtual bool auxOutput(int k, const float*& L, const float*& R) const {
    (void)k;
    (void)L;
    (void)R;
    return false;
  }
  // Controller instruments: current control outputs (0..1), as listed in
  // PluginInfo::outputs. Returns how many were written. Read after process().
  virtual int controlOutputs(float* out, int max) const {
    (void)out;
    (void)max;
    return 0;
  }

 protected:
  virtual void onNoteOn(uint32_t id, double pitch, float velocity) = 0;
  // Note-on with properties. Instruments that place, filter or modulate each
  // voice override this (and handlesNotePan() when they pan voices
  // themselves); the default plays a plain note and the base class pans the
  // instrument's output by the most recent note's pan.
  virtual void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) {
    (void)props;
    onNoteOn(id, pitch, velocity);
  }
  virtual bool handlesNotePan() const { return false; }
  virtual void onNoteOff(uint32_t id) = 0;
  // Cut: default is a normal release.
  virtual void onCut(uint32_t id) { onNoteOff(id); }
  virtual void onGlide(uint32_t id, double pitch, double seconds) {
    (void)id;
    (void)pitch;
    (void)seconds;
  }
  virtual void onMidi(uint8_t status, uint8_t d1, uint8_t d2) {
    (void)status;
    (void)d1;
    (void)d2;
  }
  virtual void onAllOff() = 0;
  virtual void onKill() = 0;
  // Called once per process() before rendering, to read parameters.
  virtual void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) = 0;
  virtual void render(float* L, float* R, int n) = 0;  // adds
  // Hosted plugins: every event of the block fires first (eventOffset() is
  // its sample offset while it fires), then one render() covers the whole
  // block — the plugin places the events itself, sample-accurately.
  virtual bool wholeBlock() const { return false; }
  int eventOffset() const { return evOffset_; }

 public:
  // MIDI a hosted instrument sent during the last process() (arpeggiators,
  // MIDI effects); the engine routes it to the channels listening on the
  // instrument's MIDI output port. Returns how many were written.
  struct MidiOutEvent {
    int offset;
    uint8_t s, a, b;
  };
  virtual int takeMidiOut(MidiOutEvent* out, int max) {
    (void)out;
    (void)max;
    return 0;
  }

 private:
  int evOffset_ = 0;
  struct Ev {
    enum Kind { On, Off, AllOff, Glide, Cut, Midi };
    Kind kind = On;
    uint32_t id = 0;
    double pitch = 0;
    float velocity = 0;
    int offset = 0;
    double time = 0;  // Glide: seconds
    NoteProps props{};
    bool hasProps = false;
    static Ev make(Kind k, uint32_t id, double pitch, float velocity, int offset) {
      Ev e;
      e.kind = k;
      e.id = id;
      e.pitch = pitch;
      e.velocity = velocity;
      e.offset = offset;
      return e;
    }
  };
  void push(const Ev& e) {
    if (count_ < events_.size()) events_[count_++] = e;
  }
  void fire(const Ev& e);
  std::array<Ev, 512> events_{};
  size_t count_ = 0;
  // Output pan for instruments that do not pan their voices (see onNoteOnEx).
  bool panUsed_ = false;
  float panTarget_ = 0, panCur_ = 0;
  double sampleRate_ = 44100;
};

// Instance for a catalogue type ("gl.synth", …); nullptr for unknown types.
std::unique_ptr<Generator> createGenerator(const std::string& type);

}  // namespace gl

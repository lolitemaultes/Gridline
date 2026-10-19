// Gridline — format-neutral interface of one hosted plugin instance.
//
// Every plugin format Gridline hosts (VST3, CLAP, VST2, AU) and the
// out-of-process sandbox proxy implement Backend; the engine adapters in
// HostedPlugins.cpp (parameters, notes, state, UI pump) only talk to this
// interface.
//
// Threads: description, prepare, state, parameter text, programs, editor and
// idle() on the UI thread; the audio-thread section (input/output buffers,
// addParamChange, addEvent, process) on the audio thread only — it never
// allocates and never locks.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gl::host {

// Parameter flags (format-neutral).
enum ParamFlag : int32_t {
  kParamAutomatable = 1,
  kParamReadOnly = 2,
  kParamHidden = 4,
  kParamBypass = 8,
  kParamModulatable = 16,
  kParamProgramChange = 32,
};

struct ParamInfo {
  uint32_t id = 0;
  std::string title, units;
  int stepCount = 0;  // 0 continuous, 1 toggle, n: n+1 discrete values
  double def = 0;     // normalized default
  int32_t flags = 0;  // ParamFlag
  bool automatable() const { return (flags & kParamAutomatable) != 0; }
  bool exposed() const { return !(flags & (kParamReadOnly | kParamHidden | kParamBypass)); }
};

// Restart requests (HostEvent::Restart flags).
enum RestartFlag : int32_t {
  kRestartReload = 1,       // re-prepare (bus layout / io changed, plugin asked for a restart)
  kRestartLatency = 2,      // latency changed
  kRestartParamValues = 4,  // many parameter values changed (program load…): read them back
  kRestartParamInfo = 8,    // parameter titles / ranges changed
};

// Calls from the plugin (its editor, its controller), queued for the UI thread.
struct HostEvent {
  enum Kind { BeginEdit, PerformEdit, EndEdit, Restart, Dirty } kind = PerformEdit;
  uint32_t id = 0;
  double value = 0;  // PerformEdit: normalized value
  int32_t flags = 0;  // Restart: RestartFlag
};

// Bounded multi-producer / single-consumer queue of host events (plugins may
// report edits from the UI thread, the audio thread or their own threads).
// push never blocks and never allocates; it drops when full.
class HostEventQueue {
 public:
  static constexpr size_t kSize = 2048;  // power of two
  HostEventQueue();
  bool push(const HostEvent& e);
  bool pop(HostEvent& e);  // single consumer (UI thread)

 private:
  struct Cell {
    std::atomic<size_t> seq{0};
    HostEvent ev;
  };
  std::unique_ptr<Cell[]> cells_;
  std::atomic<size_t> head_{0}, tail_{0};
};

// Parameter value the plugin's processor reported (output parameter changes).
struct OutputValue {
  uint32_t id = 0;
  double value = 0;  // normalized
};

// Note / MIDI input event for the next process() call.
struct NoteEvent {
  enum Type : uint8_t { NoteOn, NoteOff, Tuning, Midi } type = NoteOn;
  int offset = 0;       // sample offset in the block
  int32_t noteId = -1;  // host note id (NoteOn / NoteOff / Tuning)
  int16_t key = 60;     // MIDI key
  int8_t channel = 0;   // MIDI channel 0..15
  float velocity = 0;   // 0..1
  double tuning = 0;    // NoteOn: semitones off `key`; Tuning: semitones off the started key
  uint8_t midi[3] = {0, 0, 0};  // Midi: status, data1, data2 (CC, pitch bend, pressure…)
  // NoteOn: per-note properties (piano roll) for formats with note
  // expressions: pan -1..1, modX / modY 0..1 (0.5 neutral; X → brightness).
  bool hasProps = false;
  float pan = 0, modX = 0.5f, modY = 0.5f;
};

// Transport for one process() call (plain data: also travels through the
// sandbox's shared memory).
struct Transport {
  double sampleRate = 44100, tempo = 120;
  double projectQn = 0, barQn = 0;  // song position / start of its bar, quarter notes
  int64_t projectSamples = 0;
  int64_t continuousSamples = 0;  // samples processed since the instance started
  int32_t barNumber = 0;          // bar of projectQn, from 0
  int tsNum = 4, tsDen = 4;
  bool playing = false;
  bool recording = false;
  bool looping = false;  // the transport wraps from loopEndQn to loopStartQn
  double loopStartQn = 0, loopEndQn = 0;
  bool changed = false;  // playing / position jumped since the previous block
  bool offline = false;  // rendering faster than real time (export)
  int64_t systemNanos = 0;  // monotonic clock at the block start
};

// MIDI / note output of a plugin (arpeggiators, chord generators, MIDI
// effects), read after process().
struct MidiOut {
  int offset = 0;               // sample offset in the processed block
  uint8_t data[3] = {0, 0, 0};  // status, data1, data2 (note on / off, CC, pitch bend…)
};

// MIDI output of one block (audio thread only: filled during process(),
// read back by popMidiOut right after). Fixed capacity, kept in offset order.
class MidiOutBuffer {
 public:
  static constexpr int kMax = 1024;
  void clear() { count_ = read_ = 0; }
  void push(int offset, uint8_t s, uint8_t a, uint8_t b) {
    if (count_ >= kMax) return;
    int i = count_++;
    while (i > 0 && ev_[i - 1].offset > offset) {
      ev_[i] = ev_[i - 1];
      --i;
    }
    ev_[i].offset = offset < 0 ? 0 : offset;
    ev_[i].data[0] = s;
    ev_[i].data[1] = a;
    ev_[i].data[2] = b;
  }
  bool pop(MidiOut& m) {
    if (read_ >= count_) return false;
    m = ev_[read_++];
    return true;
  }
  int size() const { return count_; }

 private:
  MidiOut ev_[kMax];
  int count_ = 0, read_ = 0;
};

// A preset of the plugin's own library (CLAP preset discovery).
struct PresetRef {
  std::string name, category, creator;
  std::string location;       // file path ("" for presets inside the plugin)
  std::string key;            // load key inside the location (may be empty)
  uint32_t locationKind = 0;  // CLAP location kind (0 file, 1 plugin)
};

// A plugin editor (native view inside a host window).
class Editor {
 public:
  virtual ~Editor() = default;
  // Size the editor wants (logical pixels).
  virtual bool size(int& w, int& h) = 0;
  virtual bool canResize() const { return false; }
  // Adjusts a proposed size to what the editor accepts (resizable editors).
  virtual void constrain(int& w, int& h) { (void)w, (void)h; }
  // Embeds into the native parent (NSView* / X11 window id). False on failure.
  virtual bool attach(void* parent) = 0;
  // The host window changed size.
  virtual void setSize(int w, int h) { (void)w, (void)h; }
  // Called every UI frame while open (editor idle for formats that need it).
  virtual void idle() {}
  // The editor lives in its own top-level window owned by another process
  // (sandbox): the host opens no window and calls attach(nullptr).
  virtual bool external() const { return false; }
  // External editors: false once the user closed the window.
  virtual bool isOpen() const { return true; }
  // The editor asks the host for a new window size (set by the host before attach).
  std::function<bool(int w, int h)> requestResize;
};

// One plugin instance.
class Backend {
 public:
  virtual ~Backend() = default;
  Backend() = default;
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  // ---- description ----
  virtual const char* format() const = 0;  // "VST3", "CLAP", "VST2", "AU", …
  virtual const std::vector<ParamInfo>& params() const = 0;
  int paramIndex(uint32_t id) const;  // index into params(), -1 when unknown
  virtual bool hasEventInput() const = 0;
  virtual int mainInputChannels() const = 0;
  virtual int mainOutputChannels() const = 0;
  virtual int sidechainChannels() const { return 0; }
  // Extra outputs beyond the main one (multi-output instruments), each up to
  // stereo; their names for routing menus.
  virtual int auxOutputCount() const { return 0; }
  virtual std::string auxOutputName(int k) const { return "Output " + std::to_string(k + 2); }
  // Per-note tuning (glides) is understood.
  virtual bool supportsTuning() const { return false; }
  // The plugin sends notes / MIDI (a MIDI output; see popMidiOut).
  virtual bool hasEventOutput() const { return false; }
  // The plugin pans notes itself from NoteEvent::pan (note expression).
  virtual bool handlesNotePan() const { return false; }
  // Non-destructive modulation (addParamMod) is understood for parameter `id`.
  virtual bool supportsParamMod(uint32_t id) const { (void)id; return false; }
  // A bypass the plugin implements itself (bypass parameter, VST2 soft
  // bypass, AU bypass property); see setBypass.
  virtual bool hasBypass() const { return false; }

  // ---- UI thread ----
  virtual bool prepare(double sampleRate, int maxBlock) = 0;
  virtual double sampleRate() const = 0;
  virtual int maxBlock() const = 0;
  virtual bool active() const = 0;
  // Re-reads layout / latency / parameter info after the plugin asked for it
  // (RestartFlag). The adapter suspends processing around this call.
  virtual void restart(int32_t flags) { (void)flags; }
  virtual int latency() const = 0;
  virtual int tailSamples() const { return 0; }
  // Plugin state as PluginState::data ("<prefix>:<base64…>"; see each format).
  virtual bool getState(std::string& data) = 0;
  virtual bool setState(const std::string& data) = 0;
  // Parameter value as the plugin (its controller / main thread side) has it.
  virtual double paramNormalized(uint32_t id) const = 0;
  // Tells the controller side about a value the engine sent (VST3 controller,
  // AU listeners); formats with one object ignore it.
  virtual void setParamNormalized(uint32_t id, double value) { (void)id, (void)value; }
  virtual std::string paramText(uint32_t id, double normalized) const = 0;
  // Pending calls from the plugin and processor-reported parameter values.
  virtual void drainHostEvents(std::vector<HostEvent>& out) = 0;
  virtual bool popOutputValue(OutputValue& v) { (void)v; return false; }
  // Programs / factory presets (unit program list, VST2 programs, AU factory presets).
  virtual int programCount() const { return 0; }
  virtual std::string programName(int index) const { (void)index; return {}; }
  // Selects a program (UI thread; the adapter suspends processing). The
  // parameter values are read back afterwards.
  virtual bool selectProgram(int index) { (void)index; return false; }
  virtual bool hasEditor() const { return false; }
  virtual std::unique_ptr<Editor> createEditor() { return nullptr; }
  // Once per UI frame (main-thread callbacks, editor idle).
  virtual void idle() {}
  // Rendering mode (UI thread; the adapter suspends processing): offline =
  // an export faster than real time. False when the format has no notion of it.
  virtual bool setOffline(bool offline) { (void)offline; return false; }
  // The plugin's own bypass (UI thread; takes effect with the next block).
  virtual bool setBypass(bool on) { (void)on; return false; }
  // Presets of the plugin's own library (CLAP preset discovery; slow, UI
  // thread: cache the result) and loading one (the adapter suspends processing).
  virtual std::vector<PresetRef> discoverPresets() { return {}; }
  virtual bool loadPreset(const PresetRef& p) { (void)p; return false; }
  // Out-of-process instances: the plugin process died.
  virtual bool crashed() const { return false; }
  virtual std::string crashReason() const { return {}; }
  // Notified (UI thread) when the instance is destroyed while an editor shows it.
  std::function<void()> onGoingAway;

  // ---- audio thread ----
  // Buffers to fill before process() (maxBlock() frames each): main input,
  // sidechain input. nullptr for channels that do not exist.
  virtual float* inputChannel(int ch) = 0;
  virtual float* sidechainChannel(int ch) { (void)ch; return nullptr; }
  // Outputs after process(): bus 0 = main, 1.. = aux outputs.
  virtual const float* outputChannel(int bus, int ch) = 0;
  virtual int outputChannels(int bus) const { return bus == 0 ? mainOutputChannels() : 0; }
  // Queues a parameter change for the next process(); false when full.
  // Changes of one parameter are queued in offset order.
  virtual bool addParamChange(uint32_t id, double normalized, int offset) = 0;
  // Queues a modulation amount (normalized; added to the value without
  // changing it) for the next process(). False when unsupported or full.
  virtual bool addParamMod(uint32_t id, double amount, int offset) { (void)id, (void)amount, (void)offset; return false; }
  virtual void addEvent(const NoteEvent& e) = 0;
  // Drops queued parameter changes and events (block skipped).
  virtual void clearInput() = 0;
  // Runs n <= maxBlock() frames with the queued input (cleared afterwards).
  // False when inactive (outputs are then silent).
  virtual bool process(int n, const Transport& t) = 0;
  // MIDI the plugin sent during the last process(), in offset order; false
  // when there is no more.
  virtual bool popMidiOut(MidiOut& m) { (void)m; return false; }

 protected:
  // Call after (re)reading params(): rebuilds the id → index lookup.
  void indexParams();

 private:
  std::vector<std::pair<uint32_t, int>> lookup_;
};

// Shared helpers.
std::string base64Encode(const std::string& bytes);
bool base64Decode(const std::string& text, std::string& bytes);

}  // namespace gl::host

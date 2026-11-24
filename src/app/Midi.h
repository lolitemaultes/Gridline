// Gridline — MIDI device I/O (RtMidi: ALSA/JACK on Linux, CoreMIDI on macOS).
// Input goes straight to the audio engine (low latency) and is also queued for
// the UI (recording, controller links, templates). Also: external clock
// following, MMC transport, and the MIDI sync output sender thread.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../core/MidiOpen.h"
#include "../core/MidiSync.h"
#include "../core/Remote.h"

class RtMidiIn;
class RtMidiOut;

namespace gl {

class AudioEngine;

struct MidiEvent {
  uint8_t status = 0, data1 = 0, data2 = 0;
  std::string port;
  int portNumber = -1;
  double time = 0;  // K-226: when it arrived (MidiManager::clockNow seconds, stamped on the driver thread)
  bool surfacePad = false;  // a control surface's pad, played as this note (SurfacePads); `port` is the surface's
};

// A SysEx message from an input (control surfaces read their devices' answers from these).
struct SysexEvent {
  std::string port;
  std::vector<uint8_t> bytes;
  double time = 0;
};

// Control surfaces (core/surfaces): a port a surface owns. Everything it sends goes to the UI for the
// surface to read, and nothing plays straight from it. The pads the surface turns into notes (a keys or
// drums page) are translated here, on the driver thread, so they play with no added delay: a note `n`
// on MIDI channel `channel` plays `pitch[n]` (-1: not a note), its poly pressure follows it, and the
// note-off ends the pitch the note-on played even if the map changed in between.
struct SurfacePads {
  std::array<std::atomic<int16_t>, 128> pitch;
  std::atomic<int> channel{0};
  std::atomic<int> velocity{0};  // VelocityCurve for the pads
  std::array<int16_t, 128> held{};  // (driver thread) the pitch each pad's note-on played
  SurfacePads() {
    for (auto& p : pitch) p.store(-1);
    held.fill(-1);
  }
};

// Per-input settings (Settings → MIDI → input devices).
struct MidiInputConfig {
  std::string name;
  bool enabled = true;
  int port = 0;                  // port number links can filter on
  std::string controllerType;    // template id; "" = automatic (by device name), "none" = no template
  bool notes = true;             // play notes from this device
  std::string surface;           // control surface options of this device ("key=value;..."), "" = defaults
};

// Global input routing (Settings → MIDI → input).
struct MidiRouting {
  bool byChannel = false;         // false: every MIDI channel plays the selected channel
  int route[16];                  // byChannel: rack channel index per MIDI channel (-1 = selected channel)
  float bendRange[16];            // semitones
  int omniChannel = -1;           // 0..15: this MIDI channel always plays the selected channel
  VelocityCurve velocity = VelocityCurve::Linear;
  int fixedVelocity = 100;
  bool ignoreNoteOff = false;     // one-shot pads: note-offs are dropped
  bool sustain = true;            // Sustain holds notes: Gridline holds note-offs while CC 64 is down
  bool sostenuto = true, soft = true;
  float softFactor = 0.6f;
  bool mpe = false;               // K-144: MPE controller (channel 1 = master, 2..16 = one note each)
  float mpeBendRange = 48.0f;     // its per-note pitch bend range, semitones
  MidiRouting() {
    for (int i = 0; i < 16; ++i) {
      route[i] = -1;
      bendRange[i] = 2.0f;
    }
  }
};

class MidiManager {
 public:
  // `systemPorts` false (MIDI-1118: hidden / scripted test runs): the system MIDI service is never
  // asked and no device port is ever opened (no inputs, no outputs, no controller LED outputs), so
  // a connected keyboard or stray device traffic cannot reach a test. The service then reads as
  // answered with no devices; dev MIDI injection (onMessage) still works.
  explicit MidiManager(AudioEngine& engine, bool systemPorts = true);
  ~MidiManager();

  // Refreshes the port lists (keeps the settings of known devices). The system MIDI service
  // is asked on a background thread (it can hang or refuse): update() takes the answer.
  void scan();
  // Every frame (UI thread): takes the service's answer, opens the ports once it is
  // available, retries on its own while it is not. True when the port lists changed.
  // MIDIHP-1119: while the service answers, the device lists are compared again in the
  // background (at once when the system reports a change, else on a timer): a device plugged
  // in opens as at startup (per its settings; a new one enabled), one unplugged is closed and
  // the notes it held are released; plugged in again, it opens again.
  bool update();
  // K-142 (0.7.8): seconds until update() has timed work of its own (the next look for devices
  // plugged in or out, a retry of a service that refused); 1e300 when none. A hidden interface
  // loop sleeps until then at most (what comes sooner wakes it: UiWake.h).
  double secondsToNextLook(double now) const;
  // Startup: gives a working service up to `seconds` to answer, then takes its answer.
  void waitForService(double seconds);
  // The Retry button: asks the service again now.
  void retry();
  MidiOpenState serviceState() const { return opener_->state(); }
  const std::string& serviceError() const { return opener_->error(); }
  int serviceAttempts() const { return opener_->attempts(); }
  double serviceRetryIn() const { return opener_->nextRetryIn(clockNow()); }
  bool serviceAvailable() const { return opener_->state() == MidiOpenState::Ready; }
  double serviceAge() const { return clockNow() - createdAt_; }  // seconds since the service was first asked
  // Opens the enabled inputs and the selected output (only once the service answered).
  void apply();
  void load(const std::string& configFile);  // midi.json (or the old midi.txt)
  void save(const std::string& configFile) const;
  // Events received since the last call (UI thread).
  std::vector<MidiEvent> poll();
  // Transport actions from MMC sysex since the last call.
  std::vector<RemoteAction> pollActions();
  // SysEx messages received since the last call (UI thread; at most 64 kept between calls).
  std::vector<SysexEvent> pollSysex();
  // Control surfaces: `port` belongs to a surface (see SurfacePads); release gives it back.
  void claimSurfacePort(const std::string& port, std::shared_ptr<SurfacePads> pads);
  void releaseSurfacePort(const std::string& port);
  void send(uint8_t s, uint8_t a, uint8_t b);  // to the selected output
  void sendRaw(const std::vector<unsigned char>& bytes);
  // J-150: to the output of a given device (controller LEDs), opened on first
  // use; the output named like the input, or one whose name contains it.
  void sendTo(const std::string& device, const std::vector<unsigned char>& bytes);

  MidiInputConfig* inputConfig(const std::string& name);
  const MidiInputConfig* inputConfig(const std::string& name) const;
  std::vector<std::string> inputs, outputs;  // present devices
  std::vector<MidiInputConfig> inputCfg;     // known devices (present or not)
  std::string output;                        // "" = none
  MidiRouting routing;
  // Sync.
  SyncOut syncOut = SyncOut::Off;
  int syncOffsetMs = 0;        // master sync offset (positive = later)
  // MI-02: following an external master (MIDI clock, or MIDI time code).
  SyncSource syncSource = SyncSource::Internal;
  std::string clockInDevice;   // the sync input ("" = any enabled input)
  int clockInOffsetMs = 0;     // local position = master position + offset
  int chaseToleranceMs = 40;   // drift beyond this relocates the transport
  std::atomic<double> outputLatency{0.0};  // audio output latency (s), set by the app
  // Under followerMutex(): the clock follower and the time code receiver
  // (fed by the driver threads), and the chase state (UI thread).
  MidiClockFollower& follower() { return follower_; }
  MtcReceiver& mtc() { return mtc_; }
  SyncChase& chase() { return chase_; }
  std::mutex& followerMutex() { return followerMu_; }
  // The clock message times are stamped with (seconds).
  static double clockNow();

  std::string status;
  int openInputs() const { return static_cast<int>(ins_.size()); }
  std::vector<std::string> openInputNames() const;  // in the order they opened
  int rescans() const { return opener_->rescans(); }  // background comparisons of the device lists
  // Dev checks (GRIDLINE_TEST_MIDI=fake): a message as if the open input `device` sent it (its
  // driver callback). False when no such input is open.
  bool deviceMessage(const std::string& device, const std::vector<unsigned char>& msg);
  int heldNotes(const std::string& device) const;  // notes the open input holds down now
  // k078-flkey (GRIDLINE_TEST_MIDI=fake): what sendTo sent to the stand-in output `device`, in
  // order (`clear`: forgotten after reading).
  std::vector<std::vector<unsigned char>> fakeSent(const std::string& device, bool clear);
  int releasedNotes() const { return releasedNotes_; }  // note-offs sent for inputs that closed
  bool outputOpen();  // the selected output port is open
  int openDeviceOutputs();  // sendTo ports open (controller LEDs)
  bool systemPorts() const { return systemPorts_; }
  double lastActivity() const { return lastActivity_.load(); }

  // RtMidi callback trampoline (driver thread).
  // `syncPort`: the port is the chosen sync input.
  void onMessage(const std::string& port, int portNumber, const std::vector<unsigned char>& msg, bool syncPort = true);
  struct Port;

  // Copies routing / sync settings to the driver threads and the engine
  // (call after editing them). `rack` = channel ids in rack order.
  void publish(const std::vector<uint32_t>& rack);

 private:
  void senderLoop();
  std::atomic<int> velCurve_{0}, fixedVel_{100}, syncOffsetUs_{0};
  std::atomic<bool> ignoreOff_{false};
  std::atomic<int> syncSrc_{0};  // SyncSource for the driver threads
  AudioEngine& engine_;
  bool systemPorts_ = true;
  std::vector<std::unique_ptr<Port>> ins_;
  std::unique_ptr<RtMidiOut> out_;
  std::vector<std::pair<std::string, std::unique_ptr<RtMidiOut>>> devOuts_;  // sendTo (under outMu_)
  std::mutex outMu_;
  std::mutex mu_;
  std::vector<MidiEvent> pending_;
  std::vector<RemoteAction> actions_;
  std::vector<SysexEvent> sysex_;  // (under mu_)
  std::mutex surfMu_;
  std::map<std::string, std::shared_ptr<SurfacePads>> surfaces_;  // (under surfMu_)
  std::atomic_flag engineLock_ = ATOMIC_FLAG_INIT;  // several inputs → one engine queue
  std::atomic<double> lastActivity_{-1};
  // K-142 (0.7.8 follow-up): when the input last woke the interface for a stream of sync messages
  // (clock, time code): at most ten times a second, as its idle frames took them before.
  std::atomic<double> syncPokeAt_{-1};
  void pokeSync(double t);
  MidiClockFollower follower_;
  MtcReceiver mtc_;
  SyncChase chase_;
  std::mutex followerMu_;
  WakeSignal senderWake_;  // K-142: the sender's sleep (posted by the engine's ring and to quit)
  std::thread sender_;
  std::atomic<bool> quit_{false};
  std::unique_ptr<MidiOpener> opener_;
  MidiOpenState lastState_ = MidiOpenState::Idle;
  double createdAt_ = clockNow();
  void adopt(MidiProbeResult r);
  // MIDIHP-1119: opens / closes ports to match the device lists (only what changed).
  void reconcile(const std::vector<std::string>& prevIns, const std::vector<std::string>& prevOuts, bool full);
  bool openInput(const std::string& name);  // pushed on ins_ when it opened
  void closeInput(Port& p);                 // closes it, then releases the notes it held
  bool openOutput();                        // the selected output (under outMu_)
  bool fake_ = false;     // GRIDLINE_TEST_MIDI=fake: stand-in devices, no MIDI client
  bool fakeOut_ = false;  // the stand-in selected output is open (under outMu_)
  std::map<std::string, std::vector<std::vector<unsigned char>>> fakeSent_;  // k078-flkey (under outMu_)
  bool answered_ = false;  // a probe answered since the last failure: later answers change only what changed
  double nextRescan_ = 0;
  uint64_t seenChanges_ = 0;
  int releasedNotes_ = 0;
};

}  // namespace gl

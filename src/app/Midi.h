// Gridline — MIDI device I/O (RtMidi: ALSA/JACK on Linux, CoreMIDI on macOS).
// Input goes straight to the audio engine (low latency) and is also queued for
// the UI (recording, controller links, templates). Also: external clock
// following, MMC transport, and the MIDI sync output sender thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
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
};

// Per-input settings (Settings → MIDI → input devices).
struct MidiInputConfig {
  std::string name;
  bool enabled = true;
  int port = 0;                  // port number links can filter on
  std::string controllerType;    // template id; "" = automatic (by device name), "none" = no template
  bool notes = true;             // play notes from this device
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
  bool sostenuto = true, soft = true;
  float softFactor = 0.6f;
  MidiRouting() {
    for (int i = 0; i < 16; ++i) {
      route[i] = -1;
      bendRange[i] = 2.0f;
    }
  }
};

class MidiManager {
 public:
  explicit MidiManager(AudioEngine& engine);
  ~MidiManager();

  void scan();  // refresh the port lists (keeps the settings of known devices)
  // Opens the enabled inputs and the selected output.
  void apply();
  void load(const std::string& configFile);  // midi.json (or the old midi.txt)
  void save(const std::string& configFile) const;
  // Events received since the last call (UI thread).
  std::vector<MidiEvent> poll();
  // Transport actions from MMC sysex since the last call.
  std::vector<RemoteAction> pollActions();
  void send(uint8_t s, uint8_t a, uint8_t b);  // to the selected output
  void sendRaw(const std::vector<unsigned char>& bytes);

  MidiInputConfig* inputConfig(const std::string& name);
  const MidiInputConfig* inputConfig(const std::string& name) const;
  std::vector<std::string> inputs, outputs;  // present devices
  std::vector<MidiInputConfig> inputCfg;     // known devices (present or not)
  std::string output;                        // "" = none
  MidiRouting routing;
  // Sync.
  SyncOut syncOut = SyncOut::Off;
  int syncOffsetMs = 0;        // master sync offset (positive = later)
  bool clockIn = false;        // follow an external MIDI clock
  std::string clockInDevice;   // "" = any enabled input
  int clockInOffsetMs = 0;
  std::atomic<double> outputLatency{0.0};  // audio output latency (s), set by the app
  MidiClockFollower& follower() { return follower_; }
  std::mutex& followerMutex() { return followerMu_; }

  std::string status;
  int openInputs() const { return static_cast<int>(ins_.size()); }
  double lastActivity() const { return lastActivity_.load(); }

  // RtMidi callback trampoline (driver thread).
  void onMessage(const std::string& port, int portNumber, const std::vector<unsigned char>& msg);
  struct Port;

  // Copies routing / sync settings to the driver threads and the engine
  // (call after editing them). `rack` = channel ids in rack order.
  void publish(const std::vector<uint32_t>& rack);

 private:
  void senderLoop();
  std::atomic<int> velCurve_{0}, fixedVel_{100}, syncOffsetUs_{0};
  std::atomic<bool> ignoreOff_{false}, clockIn_{false};
  AudioEngine& engine_;
  std::vector<std::unique_ptr<Port>> ins_;
  std::unique_ptr<RtMidiOut> out_;
  std::mutex outMu_;
  std::mutex mu_;
  std::vector<MidiEvent> pending_;
  std::vector<RemoteAction> actions_;
  std::atomic_flag engineLock_ = ATOMIC_FLAG_INIT;  // several inputs → one engine queue
  std::atomic<double> lastActivity_{-1};
  MidiClockFollower follower_;
  std::mutex followerMu_;
  std::thread sender_;
  std::atomic<bool> quit_{false};
};

}  // namespace gl

// Gridline - control surface profiles in the app (core/surfaces): controllers that need a handshake,
// LED feedback and pages of their own, beside Mackie Control / HUI (MackieUi.h).
//
// Each profile registers a Type: its controller type id (Settings > MIDI > Controller type, kept in
// midi.json), its name in the list, how it recognises its port by name (automatic detection), and a
// factory for one Device per port. This file runs them: a device starts (handshake) when its port is
// open and set to (or detected as) the type, stops (standalone again) when the port goes, the type
// changes, the input is switched off or Gridline quits, and starts again after a rescan or replug.
// The port is claimed from the MIDI driver (MidiManager::claimSurfacePort): its messages reach the
// device only; pads the device turns into notes play through the driver's pad map.
// The FLkey keyboards (FlkeyUi.h) keep their own runner for now: one type per model, a HUI fallback
// and per-model number overrides. They could become Types here (a Device wrapping flkey::Surface).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../core/Remote.h"

namespace gl {

class App;
struct MidiEvent;
struct MidiInputConfig;
struct SurfacePads;

namespace surfaceui {

using Bytes = std::vector<uint8_t>;

// One running device (one port).
class Device {
 public:
  virtual ~Device() = default;
  virtual void connect() = 0;     // handshake
  virtual void disconnect() = 0;  // standalone again
  virtual void midi(const MidiMessage& m, double now) = 0;
  virtual void sysex(const Bytes& b, double now) = 0;
  virtual void idle(double now) = 0;  // about 30 times a second
  // Options changed in Settings (the input's `surface` string).
  virtual void options(const std::string& saved) = 0;
  // Settings > MIDI: this device's own options; true when `saved` changed.
  virtual bool settingsUi(std::string& saved) = 0;
  virtual std::string status() const = 0;  // a line for Settings and the dev commands
  // The pads the device plays (null: none). The device updates it; it is handed to the MIDI driver.
  virtual std::shared_ptr<SurfacePads> pads() { return nullptr; }
};

struct Type {
  std::string id;     // controller type id ("launchpad-pro-mk3")
  std::string label;  // shown in the list
  std::function<bool(const std::string& port)> detect;  // automatic: this port is the device's control port
  std::function<std::unique_ptr<Device>(App& a, const std::string& port, std::function<void(const Bytes&)> out)> make;
  // The output to answer on for input `port` (default: the output named like it).
  std::function<std::string(const std::string& port, const std::vector<std::string>& outputs)> output;
};

const std::vector<Type>& types();
bool isSurfaceType(const std::string& id);
const Type* findType(const std::string& id);
// The surface type a port runs: its controller type, or the detected one when the type is automatic.
const Type* typeOf(const App& a, const std::string& port);

void tick(App& a);                         // per frame (MIDI workflow)
bool midi(App& a, const MidiEvent& e);     // a message of a surface's port: true when the surface took it
bool owns(const std::string& port);        // a pad controller runs this input
void shutdown(App& a);                     // quitting: every device back to standalone
void rescanned();                          // Rescan devices: every device does its handshake again
// Settings > MIDI: the options of each running device (under the input table). True when changed.
bool settings(App& a);
// Dev input: surface [list] | surface log [off] | surface sent [clear] | surface clear
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace surfaceui
}  // namespace gl

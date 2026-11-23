// Gridline — K-214: Mackie Control / HUI surfaces in the app. Settings → MIDI → a device's
// controller type picks the protocol and the unit ("mackie" main unit, "mackie-xt1..3"
// extenders to its right, "mackie-one" one strip, "hui"); the units of one surface share its
// banks. Messages from those devices go to the surface; its replies go back to each unit's
// own output (the output named like the input).
#include "MackieUi.h"
#include <chrono>
#include <map>
#include <memory>
#include "App.h"
#include "Midi.h"
#include "MixerMeters.h"
#include "ScriptUi.h"
#include "../core/Log.h"
#include "../core/MackieControl.h"

namespace gl {

namespace {

struct Rig {
  std::string key;  // the protocol / layout it was built for
  std::unique_ptr<script::ControllerHost> host;
  std::unique_ptr<mackie::Surface> surface;
  std::map<int, std::string> unitDevice;  // unit -> MIDI device
  double lastIdle = 0;
};
Rig rig;

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Unit of a controller type (-1: not a Mackie type).
int unitOf(const std::string& type) {
  if (type == "mackie" || type == "mackie-one" || type == "hui") return 0;
  if (type.rfind("mackie-xt", 0) == 0 && type.size() == 10 && type[9] >= '1' && type[9] <= '3') return type[9] - '0';
  return -1;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& mackieControllerTypes() {
  static const std::vector<std::pair<std::string, std::string>> t = {
      {"mackie", "Mackie Control (main unit)"},          {"mackie-xt1", "Mackie Control extender 1"},
      {"mackie-xt2", "Mackie Control extender 2"},       {"mackie-xt3", "Mackie Control extender 3"},
      {"mackie-one", "Mackie Control, one strip"},       {"hui", "HUI"}};
  return t;
}

bool isMackieType(const std::string& type) { return unitOf(type) >= 0; }

void mackieTick(App& a) {
  if (!a.midi) return;
  // The surface the configured devices make up.
  std::map<int, std::string> units;
  std::string proto;
  for (const auto& name : a.midi->inputs)
    if (const MidiInputConfig* c = a.midi->inputConfig(name); c && c->enabled && isMackieType(c->controllerType)) {
      units[unitOf(c->controllerType)] = name;
      if (c->controllerType == "hui" || c->controllerType == "mackie-one" || c->controllerType == "mackie") proto = c->controllerType;
    }
  if (units.empty() || !units.count(0)) {
    rig = Rig{};
    return;
  }
  int extenders = 0;
  while (extenders < 3 && units.count(extenders + 1)) ++extenders;
  const std::string key = proto + "/" + std::to_string(extenders);
  if (!rig.surface || rig.key != key || rig.unitDevice != units) {
    rig = Rig{};
    rig.key = key;
    rig.unitDevice = units;
    rig.host = makeAppControllerHost(a, units[0]);
    mackie::Options o;
    o.protocol = proto == "hui" ? mackie::Protocol::Hui : mackie::Protocol::Mcu;
    o.extenders = proto == "mackie" ? extenders : 0;
    o.singleStrip = proto == "mackie-one";
    App* ap = &a;
    rig.surface = std::make_unique<mackie::Surface>(*rig.host, o, [ap](int unit, const std::vector<uint8_t>& b) {
      if (!ap->midi) return;
      auto it = rig.unitDevice.find(unit);
      if (it != rig.unitDevice.end()) ap->midi->sendTo(it->second, std::vector<unsigned char>(b.begin(), b.end()));
    });
    logging::info("midi", units[0] + ": " + (proto == "hui" ? "HUI" : "Mackie Control") + " surface with " + std::to_string(extenders) + " extender(s)");
  }
  const double t = wallNow();
  if (t - rig.lastIdle < 1.0 / 30.0) return;
  rig.lastIdle = t;
  std::vector<float> levels;
  for (const MixerTrack& m : a.doc.song().mixer) levels.push_back(mixmeters::get(a, m.id).peak());
  rig.surface->idle(t, levels);
}

bool mackieOwns(const std::string& device) {
  if (!rig.surface) return false;
  for (const auto& [unit, dev] : rig.unitDevice)
    if (dev == device) return true;
  return false;
}

bool mackieMidi(App& a, const std::string& device, const MidiMessage& m) {
  (void)a;
  if (!rig.surface) return false;
  for (const auto& [unit, dev] : rig.unitDevice)
    if (dev == device) {
      rig.surface->onMidi(unit, m);
      return true;  // the surface's buttons are notes: they never play
    }
  return false;
}

}  // namespace gl

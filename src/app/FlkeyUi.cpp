// Gridline - k078-flkey: Novation FLkey keyboards in the app (see FlkeyUi.h).
//
// Which ports: an enabled input whose controller type is an FLkey model, or (type Automatic) whose
// name is an FLkey's DAW port. The keyboard's MIDI port is left alone: it plays like any keyboard.
// Replies go to the output that pairs with the input (flkey::pairedOutput). Number corrections for
// a model come from <config>/controllers/flkey.json when it exists (flkey::applyOverrides).
// When an FLkey 2 answers in HUI on its DAW port, the port is handed to Gridline's HUI surface.
#include "FlkeyUi.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include "App.h"
#include "MenuParityUi.h"
#include "Midi.h"
#include "PluginUI.h"
#include "ScriptUi.h"
#include "../core/Log.h"
#include "../core/MackieControl.h"
#include "../core/surfaces/FlKey.h"

namespace gl {

namespace {

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The app as an FLkey sees it: the controller host of the device plus patterns, notes, score log.
class Host final : public flkey::Host {
 public:
  Host(App& a, const std::string& device) : flkey::Host(a.doc), a_(a), base_(makeAppControllerHost(a, device)) {}
  void action(RemoteAction act) override { base_->action(act); }
  bool playing() const override { return base_->playing(); }
  bool recording() const override { return base_->recording(); }
  bool songMode() const override { return base_->songMode(); }
  Tick position() const override { return base_->position(); }
  void setPosition(Tick t) override { base_->setPosition(t); }
  int selectedMixer() const override { return base_->selectedMixer(); }
  void selectMixer(int index) override { base_->selectMixer(index); }
  bool armed(int index) const override { return base_->armed(index); }
  void setArmed(int index, bool on) override { base_->setArmed(index, on); }
  int selectedChannel() const override { return base_->selectedChannel(); }
  void selectChannel(int index) override { base_->selectChannel(index); }
  std::pair<ObjectId, int> focusedPlugin() const override { return base_->focusedPlugin(); }
  void midiOut(uint8_t s, uint8_t d1, uint8_t d2) override { base_->midiOut(s, d1, d2); }
  void sysexOut(const std::vector<uint8_t>& b) override { base_->sysexOut(b); }
  void hint(const std::string& text) override { base_->hint(text); }
  std::string deviceName() const override { return base_->deviceName(); }
  ObjectId currentPattern() const override { return a_.curPattern; }
  void selectPattern(ObjectId p) override { a_.selectPattern(p); }
  void note(ObjectId ch, int pitch, int velocity) override {
    if (velocity > 0) {
      a_.engine.noteOn(ch, pitch, static_cast<float>(velocity) / 127.0f);
      a_.playedNoteOn(ch, pitch, velocity);
    } else {
      a_.engine.noteOff(ch, pitch);
      a_.playedNoteOff(ch, pitch);
    }
  }
  int scoreLog() override {
    const int n = menuparity::dumpScoreLogNow(a_, 5 * 60.0);  // the last five minutes
    if (n > 0) a_.notify(std::to_string(n) + " notes put into the pattern");
    return n;
  }
  double now() const override { return wallNow(); }
  std::string stepPreset(int by) override {  // the plugin window's preset arrows, on the selected channel
    const Channel* c = a_.doc.song().channel(a_.selChannel);
    if (!c) return {};
    PluginTarget t;
    t.owner = c->id;
    return stepPluginPreset(a_, t, by);
  }

 private:
  App& a_;
  std::unique_ptr<script::ControllerHost> base_;
};

struct Device {
  const flkey::Model* model = nullptr;
  std::string output;
  std::unique_ptr<Host> host;
  std::unique_ptr<flkey::Surface> surface;
  // HUI (FLkey 2 in its mode for other software).
  std::unique_ptr<script::ControllerHost> huiHost;
  std::unique_ptr<mackie::Surface> hui;
};
std::map<std::string, Device> devices;  // by input port name

// The model a configured input drives, nullptr when it is not an FLkey's DAW port.
const flkey::Model* modelFor(const std::string& name, const std::string& type) {
  const flkey::PortMatch pm = flkey::matchPort(name);
  if (type.empty()) return pm.model && pm.daw ? pm.model : nullptr;
  const flkey::Model* m = flkey::modelById(type);
  if (!m) return nullptr;
  if (pm.model && !pm.daw) return nullptr;  // picked on the keyboard's MIDI port: that port plays
  return m;
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

void send(App& a, const std::string& output, const std::vector<uint8_t>& b) {
  if (a.midi && !output.empty()) a.midi->sendTo(output, std::vector<unsigned char>(b.begin(), b.end()));
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& flkeyControllerTypes() {
  static const std::vector<std::pair<std::string, std::string>> t = [] {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& m : flkey::models()) v.emplace_back(m.id, m.name);
    return v;
  }();
  return t;
}

bool isFlkeyType(const std::string& type) { return flkey::modelById(type) != nullptr; }

std::string flkeyAutoName(const std::string& input) {
  const flkey::Model* m = modelFor(input, "");
  return m ? m->name : "";
}

void flkeyTick(App& a) {
  if (!a.midi) return;
  // The DAW ports configured now.
  std::map<std::string, const flkey::Model*> want;
  for (const auto& name : a.midi->inputs)
    if (const MidiInputConfig* c = a.midi->inputConfig(name); c && c->enabled && c->controllerType != "none")
      if (const flkey::Model* m = modelFor(name, c->controllerType)) want[name] = m;
  // Gone or changed: a keyboard still plugged in goes back to standalone.
  for (auto it = devices.begin(); it != devices.end();) {
    auto w = want.find(it->first);
    if (w != want.end() && w->second == it->second.model) {
      ++it;
      continue;
    }
    const bool present = std::find(a.midi->inputs.begin(), a.midi->inputs.end(), it->first) != a.midi->inputs.end();
    if (present && it->second.surface) it->second.surface->disconnect();
    logging::info("midi", it->first + ": " + it->second.model->name + (present ? " released" : " unplugged"));
    it = devices.erase(it);
  }
  // New: the handshake.
  std::string overrides;
  bool overridesRead = false;
  for (const auto& [name, model] : want) {
    if (devices.count(name)) continue;
    if (!overridesRead) overrides = readFile(a.configDir() + "/controllers/flkey.json"), overridesRead = true;
    Device d;
    d.model = model;
    d.output = flkey::pairedOutput(name, a.midi->outputs);
    d.host = std::make_unique<Host>(a, name);
    flkey::Wire w = flkey::defaultWire(*model);
    std::string err;
    if (!overrides.empty() && !flkey::applyOverrides(w, *model, overrides, &err)) logging::warning("midi", "controllers/flkey.json: " + err);
    const std::string out = d.output;
    App* ap = &a;
    d.surface = std::make_unique<flkey::Surface>(*d.host, *model, w, [ap, out](const std::vector<uint8_t>& b) { send(*ap, out, b); });
    d.surface->connect();
    logging::info("midi", name + ": " + model->name + " (DAW port), replies to " + (out.empty() ? std::string("no output found") : out));
    if (flkey::matchPort(name).sizeGuessed && !flkey::modelById(a.midi->inputConfig(name)->controllerType))
      logging::info("midi", name + ": the port name has no size; taken for the 49 (pick the model in Settings > MIDI)");
    devices.emplace(name, std::move(d));
  }
  // Feedback, about 30 times a second (the surfaces throttle and send only changes).
  const double t = wallNow();
  for (auto& [name, d] : devices) {
    if (d.surface && d.surface->huiSeen() && !d.hui) {  // FLkey 2 in HUI: the HUI surface takes the port
      d.huiHost = makeAppControllerHost(a, name);
      const std::string out = d.output;
      App* ap = &a;
      d.hui = std::make_unique<mackie::Surface>(*d.huiHost, mackie::Options{mackie::Protocol::Hui, 0, false},
                                                [ap, out](int, const std::vector<uint8_t>& b) { send(*ap, out, b); });
      logging::info("midi", name + ": the keyboard answers in HUI; using the HUI surface");
    }
    if (d.hui) {
      d.hui->idle(t);
    } else if (d.surface) {
      d.surface->idle(t);
    }
  }
}

bool flkeyMidi(App& a, const std::string& device, const MidiMessage& m) {
  (void)a;
  auto it = devices.find(device);
  if (it == devices.end()) return false;
  if (it->second.hui) {
    it->second.hui->onMidi(0, m);
    return true;
  }
  char hex[48];
  std::snprintf(hex, sizeof hex, "%02X %02X %02X", m.status, m.d1, m.d2);
  logging::verbose("flkey", device + " in: " + hex);  // Settings > Debug, detail Verbose: every message (hardware checks)
  it->second.surface->onMidi(m);
  return true;  // the DAW port carries no notes to play
}

bool flkeyOwns(const std::string& device) { return devices.count(device) != 0; }

void flkeyShutdown() {
  for (auto& [name, d] : devices)
    if (d.surface) d.surface->disconnect();
  devices.clear();
}

std::vector<std::string> flkeyDescribe() {
  std::vector<std::string> out;
  for (const auto& [name, d] : devices) {
    const flkey::Surface& s = *d.surface;
    char buf[512];
    std::snprintf(buf, sizeof buf, "\"%s\" model %s output \"%s\" connected %d hui %d page %s knobs \"%s\" step-page %d channel-bank %d mixer-bank %d pads",
                  name.c_str(), d.model->id, d.output.c_str(), s.connected() ? 1 : 0, d.hui ? 1 : 0, flkey::padPageName(s.page()),
                  flkey::knobModeName(s.knobMode()), s.stepPage(), s.channelBank(), s.mixerBank());
    std::string line = buf;
    for (int i = 0; i < 16; ++i) {
      std::snprintf(buf, sizeof buf, " %06X", static_cast<unsigned>(s.padColour(i)));
      line += buf;
    }
    const auto k1 = s.controlText(0), f1 = s.controlText(8);
    line += " channel " + std::to_string(d.host->selectedChannel()) + " knob1 \"" + k1.first + "=" + k1.second + "\" fader1 \"" + f1.first + "=" + f1.second + "\"";
    out.push_back(line);
  }
  return out;
}

}  // namespace gl

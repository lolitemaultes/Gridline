#include "Midi.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include "../../third_party/rtmidi/RtMidi.h"
#include "MidiBackend.h"
#include "../core/Json.h"
#include "../core/MidiTemplates.h"
#include "../core/Project.h"
#include "../engine/AudioEngine.h"

namespace gl {

struct MidiManager::Port {
  std::unique_ptr<RtMidiIn> in;
  std::string name;
  int number = 0;
  bool notes = true;
  bool clockSource = false;
  MidiManager* owner = nullptr;
};

namespace {
void callback(double, std::vector<unsigned char>* msg, void* user) {
  auto* p = static_cast<MidiManager::Port*>(user);
  if (msg && p && p->owner) p->owner->onMessage(p->name, p->notes ? p->number : -(p->number + 2), *msg, p->clockSource);
}
double nowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

double MidiManager::clockNow() { return nowSec(); }

MidiManager::MidiManager(AudioEngine& engine) : engine_(engine) {
  sender_ = std::thread([this] { senderLoop(); });
}

MidiManager::~MidiManager() {
  quit_ = true;
  if (sender_.joinable()) sender_.join();
  for (auto& p : ins_)
    if (p->in) p->in->closePort();
  ins_.clear();
  std::lock_guard<std::mutex> lk(outMu_);
  if (out_) out_->closePort();
}

MidiInputConfig* MidiManager::inputConfig(const std::string& name) {
  for (auto& c : inputCfg)
    if (c.name == name) return &c;
  return nullptr;
}
const MidiInputConfig* MidiManager::inputConfig(const std::string& name) const {
  for (const auto& c : inputCfg)
    if (c.name == name) return &c;
  return nullptr;
}

void MidiManager::scan() {
  inputs.clear();
  outputs.clear();
  try {
    auto probe = makeMidiIn();
    for (unsigned i = 0; i < probe->getPortCount(); ++i) inputs.push_back(probe->getPortName(i));
    auto probeOut = makeMidiOut();
    for (unsigned i = 0; i < probeOut->getPortCount(); ++i) outputs.push_back(probeOut->getPortName(i));
    status = std::to_string(inputs.size()) + " input(s), " + std::to_string(outputs.size()) + " output(s) found (" + midiBackendName() + ")";
  } catch (const RtMidiError& e) {
    status = "MIDI unavailable: " + e.getMessage();
  }
  for (const auto& n : inputs)
    if (!inputConfig(n)) {  // a new device: enabled, next free port number
      MidiInputConfig c;
      c.name = n;
      int port = 0;
      for (bool used = true; used; ++port) {
        used = false;
        for (const auto& x : inputCfg) used = used || x.port == port;
        if (!used) break;
      }
      c.port = port;
      inputCfg.push_back(c);
    }
}

void MidiManager::apply() {
  for (auto& p : ins_)
    if (p->in) p->in->closePort();
  ins_.clear();
  try {
    for (const auto& name : inputs) {
      const MidiInputConfig* cfg = inputConfig(name);
      if (!cfg || !cfg->enabled) continue;
      auto port = std::make_unique<Port>();
      port->in = makeMidiIn();
      port->name = name;
      port->number = cfg->port;
      port->notes = cfg->notes;
      port->clockSource = clockInDevice.empty() || clockInDevice == name;
      port->owner = this;
      // Port indices can shift between scans: look the name up again.
      for (unsigned k = 0; k < port->in->getPortCount(); ++k)
        if (port->in->getPortName(k) == name) {
          port->in->openPort(k, "Gridline In");
          break;
        }
      if (!port->in->isPortOpen()) continue;
      // Sysex for MMC; timing only when following an external clock.
      port->in->ignoreTypes(false, !(syncSource != SyncSource::Internal && port->clockSource), true);
      port->in->setCallback(callback, port.get());
      ins_.push_back(std::move(port));
    }
    std::lock_guard<std::mutex> lk(outMu_);
    if (out_) out_->closePort();
    out_.reset();
    if (!output.empty()) {
      out_ = makeMidiOut();
      for (unsigned k = 0; k < out_->getPortCount(); ++k)
        if (out_->getPortName(k) == output) {
          out_->openPort(k, "Gridline Out");
          break;
        }
    }
    status = std::to_string(ins_.size()) + " input(s) open" + (out_ && out_->isPortOpen() ? ", output open" : "");
  } catch (const RtMidiError& e) {
    status = "MIDI error: " + e.getMessage();
  }
}

void MidiManager::publish(const std::vector<uint32_t>& rack) {
  velCurve_ = static_cast<int>(routing.velocity);
  fixedVel_ = routing.fixedVelocity;
  ignoreOff_ = routing.ignoreNoteOff;
  syncSrc_ = static_cast<int>(syncSource);
  syncOffsetUs_ = syncOffsetMs * 1000;
  for (int c = 0; c < 16; ++c) {
    ObjectId id = kNoId;
    if (routing.byChannel && c != routing.omniChannel && routing.route[c] >= 0 &&
        routing.route[c] < static_cast<int>(rack.size()))
      id = rack[static_cast<size_t>(routing.route[c])];
    engine_.setMidiChannelRoute(c, id);
    engine_.setBendRange(c, routing.bendRange[c]);
  }
  engine_.setPedals(routing.sostenuto, routing.soft, routing.softFactor);
  engine_.setSyncOut(syncOut);
}

void MidiManager::load(const std::string& file) {
  std::ifstream in(file);
  if (!in) {  // older versions: midi.txt with "in:" / "out:" lines
    std::string txt = file;
    if (txt.size() > 5 && txt.compare(txt.size() - 5, 5, ".json") == 0) txt = txt.substr(0, txt.size() - 5) + ".txt";
    std::ifstream old(txt);
    for (std::string l; std::getline(old, l);) {
      if (l.rfind("in:", 0) == 0) {
        MidiInputConfig c;
        c.name = l.substr(3);
        c.port = static_cast<int>(inputCfg.size());
        inputCfg.push_back(c);
      }
      if (l.rfind("out:", 0) == 0) output = l.substr(4);
    }
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) return;
  inputCfg.clear();
  if (const Json* a = j.find("inputs"); a && a->is(Json::Type::Array))
    for (const auto& x : a->array) {
      MidiInputConfig c;
      c.name = x.text("name");
      c.enabled = x.num("enabled", 1) != 0;
      c.port = jsonInt<int>(x.num("port", 0));
      c.controllerType = x.text("controller");
      c.notes = x.num("notes", 1) != 0;
      if (!c.name.empty()) inputCfg.push_back(c);
    }
  output = j.text("output");
  syncOut = static_cast<SyncOut>(std::clamp(jsonInt<int>(j.num("syncOut", 0)), 0, static_cast<int>(SyncOut::Count) - 1));
  syncOffsetMs = jsonInt<int>(j.num("syncOffsetMs", 0));
  // MI-02: sync source (older configs: "clockIn" = follow a MIDI clock).
  syncSource = static_cast<SyncSource>(std::clamp(jsonInt<int>(j.num("syncSource", j.num("clockIn", 0) != 0 ? 1 : 0)), 0,
                                                  static_cast<int>(SyncSource::Count) - 1));
  chaseToleranceMs = std::clamp(jsonInt<int>(j.num("chaseToleranceMs", 40)), 5, 1000);
  clockInDevice = j.text("clockInDevice");
  clockInOffsetMs = jsonInt<int>(j.num("clockInOffsetMs", 0));
  MidiRouting& r = routing;
  r.byChannel = j.num("byChannel", 0) != 0;
  r.omniChannel = jsonInt<int>(j.num("omniChannel", -1));
  r.velocity = static_cast<VelocityCurve>(std::clamp(jsonInt<int>(j.num("velocityCurve", 0)), 0, static_cast<int>(VelocityCurve::Count) - 1));
  r.fixedVelocity = std::clamp(jsonInt<int>(j.num("fixedVelocity", 100)), 1, 127);
  r.ignoreNoteOff = j.num("ignoreNoteOff", 0) != 0;
  r.sostenuto = j.num("sostenuto", 1) != 0;
  r.soft = j.num("soft", 1) != 0;
  r.softFactor = static_cast<float>(std::clamp(j.num("softFactor", 0.6), 0.1, 1.0));
  if (const Json* a = j.find("route"); a && a->is(Json::Type::Array))
    for (size_t i = 0; i < a->array.size() && i < 16; ++i) r.route[i] = static_cast<int>(a->array[i].number);
  if (const Json* a = j.find("bendRange"); a && a->is(Json::Type::Array))
    for (size_t i = 0; i < a->array.size() && i < 16; ++i)
      r.bendRange[i] = static_cast<float>(std::clamp(a->array[i].number, 0.0, 48.0));
}

void MidiManager::save(const std::string& file) const {
  Json j = Json::makeObject();
  Json ins = Json::makeArray();
  for (const auto& c : inputCfg) {
    Json x = Json::makeObject();
    x.object["name"] = Json::makeString(c.name);
    x.object["enabled"] = Json::makeNumber(c.enabled ? 1 : 0);
    x.object["port"] = Json::makeNumber(c.port);
    x.object["controller"] = Json::makeString(c.controllerType);
    x.object["notes"] = Json::makeNumber(c.notes ? 1 : 0);
    ins.array.push_back(std::move(x));
  }
  j.object["inputs"] = std::move(ins);
  j.object["output"] = Json::makeString(output);
  j.object["syncOut"] = Json::makeNumber(static_cast<int>(syncOut));
  j.object["syncOffsetMs"] = Json::makeNumber(syncOffsetMs);
  j.object["syncSource"] = Json::makeNumber(static_cast<int>(syncSource));
  j.object["chaseToleranceMs"] = Json::makeNumber(chaseToleranceMs);
  j.object["clockInDevice"] = Json::makeString(clockInDevice);
  j.object["clockInOffsetMs"] = Json::makeNumber(clockInOffsetMs);
  const MidiRouting& r = routing;
  j.object["byChannel"] = Json::makeNumber(r.byChannel ? 1 : 0);
  j.object["omniChannel"] = Json::makeNumber(r.omniChannel);
  j.object["velocityCurve"] = Json::makeNumber(static_cast<int>(r.velocity));
  j.object["fixedVelocity"] = Json::makeNumber(r.fixedVelocity);
  j.object["ignoreNoteOff"] = Json::makeNumber(r.ignoreNoteOff ? 1 : 0);
  j.object["sostenuto"] = Json::makeNumber(r.sostenuto ? 1 : 0);
  j.object["soft"] = Json::makeNumber(r.soft ? 1 : 0);
  j.object["softFactor"] = Json::makeNumber(r.softFactor);
  Json route = Json::makeArray(), bend = Json::makeArray();
  for (int i = 0; i < 16; ++i) {
    route.array.push_back(Json::makeNumber(r.route[i]));
    bend.array.push_back(Json::makeNumber(r.bendRange[i]));
  }
  j.object["route"] = std::move(route);
  j.object["bendRange"] = std::move(bend);
  writeFileAtomic(file, jsonStringify(j) + "\n");
}

void MidiManager::onMessage(const std::string& port, int portNumber, const std::vector<unsigned char>& msg, bool syncPort) {
  if (msg.empty()) return;
  const double t = nowSec();
  const bool notes = portNumber >= 0;
  if (!notes) portNumber = -portNumber - 2;
  uint8_t s = msg[0], a = msg.size() > 1 ? msg[1] : 0, b = msg.size() > 2 ? msg[2] : 0;
  const auto src = static_cast<SyncSource>(syncSrc_.load());
  if (src == SyncSource::Mtc && syncPort && (s == 0xF1 || s == 0xF0)) {  // MI-02: time code (quarter / full frames)
    std::lock_guard<std::mutex> lk(followerMu_);
    if (mtc_.message(msg.data(), msg.size(), t)) return;
  }
  if (s == 0xF0) {  // sysex: MIDI Machine Control transport
    RemoteAction act = parseMmc(msg);
    if (act != RemoteAction::None) {
      std::lock_guard<std::mutex> lk(mu_);
      if (actions_.size() < 256) actions_.push_back(act);
    }
    return;
  }
  if (s >= 0xF0) {  // clock, start/continue/stop, song position
    if (src == SyncSource::Clock && syncPort) {
      std::lock_guard<std::mutex> lk(followerMu_);
      follower_.message(s, a, b, t);
    }
    return;
  }
  if (s < 0x80) return;
  const int kind = s & 0xF0;
  bool toEngine = true;
  if (kind == 0x90 && b > 0) {
    if (!notes) toEngine = false;
    b = static_cast<uint8_t>(applyVelocityCurve(static_cast<VelocityCurve>(velCurve_.load()), b, fixedVel_.load()));
  } else if (kind == 0x80 || (kind == 0x90 && b == 0)) {
    if (!notes || ignoreOff_.load()) toEngine = false;
  }
  if (toEngine) {
    while (engineLock_.test_and_set(std::memory_order_acquire)) {
    }
    engine_.midiEvent(s, a, b);
    engineLock_.clear(std::memory_order_release);
  }
  lastActivity_.store(t);
  std::lock_guard<std::mutex> lk(mu_);
  if (pending_.size() < 4096) pending_.push_back({s, a, b, port, portNumber});
}

std::vector<MidiEvent> MidiManager::poll() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<MidiEvent> out;
  out.swap(pending_);
  return out;
}

std::vector<RemoteAction> MidiManager::pollActions() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<RemoteAction> out;
  out.swap(actions_);
  return out;
}

void MidiManager::send(uint8_t s, uint8_t a, uint8_t b) {
  int size = (s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0 ? 2 : 3;
  std::vector<unsigned char> m = {s, a, b};
  m.resize(static_cast<size_t>(size));
  sendRaw(m);
}

void MidiManager::sendRaw(const std::vector<unsigned char>& m) {
  std::lock_guard<std::mutex> lk(outMu_);
  if (!out_ || !out_->isPortOpen()) return;
  try {
    out_->sendMessage(&m);
  } catch (const RtMidiError&) {
  }
}

// Sends the engine's sync messages when they are heard: the engine stamps them
// with sample positions; the block they belong to started at blockTime() and
// reaches the speakers after the output latency.
void MidiManager::senderLoop() {
  std::vector<unsigned char> bytes;
  while (!quit_.load()) {
    MidiOutEvent e;
    while (engine_.syncEvents().peek(e)) {
      const double sr = engine_.sampleRate();
      const double due = engine_.blockTime() + static_cast<double>(e.sample - engine_.blockSample()) / sr +
                         outputLatency.load() + syncOffsetUs_.load() * 1e-6;
      const double now = nowSec();
      if (due > now + 0.0005 && due < now + 1.0) break;  // not yet
      engine_.syncEvents().pop(e);
      bytes.assign(e.data, e.data + e.size);
      sendRaw(bytes);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

}  // namespace gl

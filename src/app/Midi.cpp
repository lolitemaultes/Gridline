#include "Midi.h"
#include "../core/ThreadJoin.h"
#include "../core/ThreadName.h"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <exception>
#include <fstream>
#include <sstream>
#include "../../third_party/rtmidi/RtMidi.h"
#include "MidiBackend.h"
#include "UiWake.h"
#include "../core/Json.h"
#include "../core/MidiTemplates.h"
#include "../core/Project.h"
#include "../engine/AudioEngine.h"
#include "../packs/platter/platter.h"

namespace gl {

struct MidiManager::Port {
  std::unique_ptr<RtMidiIn> in;
  bool fakeOpen = false;  // GRIDLINE_TEST_MIDI=fake: a stand-in device, no RtMidi port
  std::string name;
  int number = 0;
  bool notes = true;
  bool clockSource = false;
  MidiManager* owner = nullptr;
  // MIDIHP-1119: notes held down (bit per note, 16 channels x 128) and sustain pedals down (bit
  // per channel), written by the driver thread, so a device unplugged mid-note leaves none stuck.
  std::atomic<uint64_t> held[32] = {};
  std::atomic<uint32_t> pedal{0};
  bool open() const { return fakeOpen || (in && in->isPortOpen()); }
};

namespace {
void trackHeld(MidiManager::Port& p, const std::vector<unsigned char>& m) {
  if (m.size() < 2) {
    return;
  }
  const int kind = m[0] & 0xF0, ch = m[0] & 0x0F;
  const uint8_t d2 = m.size() > 2 ? m[2] : 0;
  if (kind == 0x90 || kind == 0x80) {
    const int key = ch * 128 + (m[1] & 0x7F);
    const uint64_t bit = uint64_t{1} << (key % 64);
    if (kind == 0x90 && d2 > 0) {
      p.held[key / 64].fetch_or(bit, std::memory_order_relaxed);
    } else {
      p.held[key / 64].fetch_and(~bit, std::memory_order_relaxed);
    }
  } else if (kind == 0xB0 && m[1] == 64) {
    if (d2 >= 64) {
      p.pedal.fetch_or(1u << ch, std::memory_order_relaxed);
    } else {
      p.pedal.fetch_and(~(1u << ch), std::memory_order_relaxed);
    }
  } else if (kind == 0xB0 && (m[1] == 123 || m[1] == 120)) {  // all notes / sound off
    p.held[ch * 2].store(0, std::memory_order_relaxed);
    p.held[ch * 2 + 1].store(0, std::memory_order_relaxed);
  }
}
void deliver(MidiManager::Port* p, const std::vector<unsigned char>& msg) {
  trackHeld(*p, msg);
  p->owner->onMessage(p->name, p->notes ? p->number : -(p->number + 2), msg, p->clockSource);
}
void callback(double, std::vector<unsigned char>* msg, void* user) {
  auto* p = static_cast<MidiManager::Port*>(user);
  if (msg && p && p->owner) {
    deliver(p, *msg);
  }
}
bool listed(const std::vector<std::string>& v, const std::string& n) { return std::find(v.begin(), v.end(), n) != v.end(); }
double nowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

double MidiManager::clockNow() { return nowSec(); }

MidiManager::MidiManager(AudioEngine& engine, bool systemPorts)
    : engine_(engine), systemPorts_(systemPorts), fake_(systemPorts && midiFakeMode()) {
  packs::platter::hub::setTapped(true);  // J-150: DJ decks read every input from the hub
  sender_ = std::thread([this] { gl::threads::setName("gl-midi-send"); senderLoop(); });
  if (systemPorts_) {
    opener_ = std::make_unique<MidiOpener>(probeMidiService);
  } else {  // MIDI-1118: no MIDI client is created; the "service" answers at once with no devices
    opener_ = std::make_unique<MidiOpener>([] {
      MidiProbeResult r;
      r.ok = true;
      return r;
    });
  }
  opener_->setOnAnswer([] { uiwake::poke(); });  // K-142 (0.7.8): the answer is taken at once, also while nothing is drawn
}

MidiManager::~MidiManager() {
  quit_ = true;
  senderWake_.post();
  gl::threads::safeJoin(sender_);
  try {
    for (auto& p : ins_)
      if (p->in) p->in->closePort();
    ins_.clear();
    std::lock_guard<std::mutex> lk(outMu_);
    if (out_) out_->closePort();
  } catch (...) {  // a driver error while closing never ends the process
  }
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
  answered_ = false;  // its answer is taken in full: a device that would not open is tried again
  opener_->start(clockNow());
  if (opener_->state() == MidiOpenState::Opening) status = "Looking for MIDI devices (" + midiBackendName() + ")";
}

void MidiManager::adopt(MidiProbeResult r) {
  inputs = std::move(r.inputs);
  outputs = std::move(r.outputs);
  status = std::to_string(inputs.size()) + " input(s), " + std::to_string(outputs.size()) + " output(s) found (" + midiBackendName() + ")";
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

bool MidiManager::update() {
  bool changed = false;
  const double now = clockNow();
  if (opener_->update(now)) {
    MidiProbeResult r = opener_->takeResult();
    if (r.ok) {
      const bool full = !answered_;
      if (full || r.inputs != inputs || r.outputs != outputs) {  // a background rescan that found nothing new changes nothing
        const std::vector<std::string> prevIns = inputs, prevOuts = outputs;
        adopt(std::move(r));
        reconcile(prevIns, prevOuts, full);
        changed = true;
      }
      answered_ = true;
      nextRescan_ = now + (midiSetupWatched() ? 10.0 : 1.5);
    } else {  // the ports already open stay as they are: the service is not asked again until it answers
      inputs.clear();
      outputs.clear();
      status = "MIDI unavailable: " + r.error;
      answered_ = false;
      changed = true;
    }
  }
  const MidiOpenState st = opener_->state();
  if (st == MidiOpenState::NotResponding && lastState_ != st) {
    inputs.clear();
    outputs.clear();
    status = "MIDI unavailable: the system MIDI service is not responding";
    answered_ = false;
    changed = true;
  }
  lastState_ = st;
  // MIDIHP-1119: devices plugged in or out. Never in hidden / scripted runs (no system ports).
  if (systemPorts_ && midiHotplugSupported() && st == MidiOpenState::Ready && !opener_->probeRunning()) {
    const uint64_t ch = midiSetupChanges();
    if (ch != seenChanges_ || now >= nextRescan_) {
      seenChanges_ = ch;
      nextRescan_ = now + (midiSetupWatched() ? 10.0 : 1.5);
      opener_->rescan(now);
    }
  }
  return changed;
}

double MidiManager::secondsToNextLook(double now) const {
  double left = 1e300;
  const MidiOpenState st = opener_->state();
  if (systemPorts_ && midiHotplugSupported() && st == MidiOpenState::Ready && !opener_->probeRunning()) {
    left = std::max(0.0, nextRescan_ - now);
  }
  const double retry = opener_->nextRetryIn(now);
  if (retry >= 0) left = std::min(left, retry);
  return left;
}

std::vector<std::string> MidiManager::openInputNames() const {
  std::vector<std::string> v;
  for (const auto& p : ins_) {
    v.push_back(p->name);
  }
  return v;
}

bool MidiManager::openInput(const std::string& name) {
  const MidiInputConfig* cfg = inputConfig(name);
  if (!cfg || !cfg->enabled) {
    return false;
  }
  for (const auto& p : ins_) {
    if (p->name == name) {
      return true;  // already open
    }
  }
  auto port = std::make_unique<Port>();
  port->name = name;
  port->number = cfg->port;
  port->notes = cfg->notes;
  port->clockSource = clockInDevice.empty() || clockInDevice == name;
  port->owner = this;
  if (fake_) {
    port->fakeOpen = midiFakePresent(false, name);
  } else {
    try {
      port->in = makeMidiIn();
      // Port indices can shift between scans: look the name up again.
      for (unsigned k = 0; k < port->in->getPortCount(); ++k) {
        if (port->in->getPortName(k) == name) {
          port->in->openPort(k, "Gridline In");
          break;
        }
      }
      if (port->in->isPortOpen()) {
        // Sysex for MMC; timing only when following an external clock.
        port->in->ignoreTypes(false, !(syncSource != SyncSource::Internal && port->clockSource), true);
        port->in->setCallback(callback, port.get());
      }
    } catch (const RtMidiError& e) {
      status = "MIDI error: " + e.getMessage();
      return false;
    } catch (const std::exception& e) {
      status = std::string("MIDI error: ") + e.what();
      return false;
    } catch (...) {
      status = "MIDI error";
      return false;
    }
  }
  if (!port->open()) {
    return false;
  }
  ins_.push_back(std::move(port));
  return true;
}

void MidiManager::closeInput(Port& p) {
  try {
    if (p.in) {
      p.in->closePort();
    }
  } catch (...) {  // a driver error while closing (the device is gone) never ends the process
  }
  p.fakeOpen = false;
  // The driver delivers no more: the notes it held down end as if released on the device.
  for (int w = 0; w < 32; ++w) {
    uint64_t bits = p.held[w].exchange(0, std::memory_order_relaxed);
    for (int b = 0; bits != 0; ++b, bits >>= 1) {
      if (bits & 1) {
        const int key = w * 64 + b;
        onMessage(p.name, p.notes ? p.number : -(p.number + 2),
                  {static_cast<unsigned char>(0x80 | (key / 128)), static_cast<unsigned char>(key % 128), 0}, p.clockSource);
        ++releasedNotes_;
      }
    }
  }
  const uint32_t pedals = p.pedal.exchange(0, std::memory_order_relaxed);
  for (int ch = 0; ch < 16; ++ch) {
    if (pedals & (1u << ch)) {
      onMessage(p.name, p.notes ? p.number : -(p.number + 2), {static_cast<unsigned char>(0xB0 | ch), 64, 0}, p.clockSource);
    }
  }
}

bool MidiManager::openOutput() {
  if (fake_) {
    fakeOut_ = !output.empty() && midiFakePresent(true, output);
    return fakeOut_;
  }
  out_ = makeMidiOut();
  for (unsigned k = 0; k < out_->getPortCount(); ++k) {
    if (out_->getPortName(k) == output) {
      out_->openPort(k, "Gridline Out");
      break;
    }
  }
  return out_->isPortOpen();
}

void MidiManager::reconcile(const std::vector<std::string>& prevIns, const std::vector<std::string>& prevOuts, bool full) {
  // Never on a service that has not answered: creating a MIDI client could hang this thread.
  if (!serviceAvailable()) {
    return;
  }
  if (!systemPorts_) {  // MIDI-1118: test runs open no device ports
    status = "MIDI devices are not opened in test runs";
    return;
  }
  // Unplugged (or no longer listed): closed, the notes they held released.
  for (auto it = ins_.begin(); it != ins_.end();) {
    if (!listed(inputs, (*it)->name)) {
      closeInput(**it);
      it = ins_.erase(it);
    } else {
      ++it;
    }
  }
  // Plugged in (or back): opened as at startup. A device that stayed listed and would not open
  // is not tried again on every rescan (Rescan, or plugging it in again, does).
  for (const auto& name : inputs) {
    if (full || !listed(prevIns, name)) {
      openInput(name);  // (no-op when it is open already)
    }
  }
  try {
    std::lock_guard<std::mutex> lk(outMu_);
    const bool isOpen = fake_ ? fakeOut_ : (out_ && out_->isPortOpen());
    if (isOpen && !listed(outputs, output)) {  // the selected output was unplugged
      if (out_) {
        out_->closePort();
      }
      out_.reset();
      fakeOut_ = false;
    } else if (!isOpen && !output.empty() && listed(outputs, output) && (full || !listed(prevOuts, output))) {
      openOutput();
    }
    // Controller LED outputs: any change of the outputs and they are looked up again on their
    // next message (one unplugged is closed, one plugged in is found).
    if (full || outputs != prevOuts) {
      for (auto& d : devOuts_) {
        if (d.second) {
          d.second->closePort();
        }
      }
      devOuts_.clear();
    }
  } catch (const RtMidiError& e) {
    status = "MIDI error: " + e.getMessage();
    return;
  } catch (...) {
    status = "MIDI error";
    return;
  }
  status = std::to_string(ins_.size()) + " input(s) open" + (outputOpen() ? ", output open" : "");
}

void MidiManager::waitForService(double seconds) {
  opener_->waitFor(seconds);
  update();
}

void MidiManager::retry() {
  opener_->retryNow(clockNow());
  update();
}

void MidiManager::apply() {
  // Never on a service that has not answered: creating a MIDI client could hang this thread.
  if (!serviceAvailable()) {
    return;
  }
  if (!systemPorts_) {  // MIDI-1118: test runs open no device ports (stand-in devices listed by dev checks included)
    status = "MIDI devices are not opened in test runs";
    return;
  }
  for (auto& p : ins_) {
    closeInput(*p);  // (the notes it held are released)
  }
  ins_.clear();
  try {
    for (const auto& name : inputs) {
      openInput(name);
    }
    std::lock_guard<std::mutex> lk(outMu_);
    if (out_) {
      out_->closePort();
    }
    out_.reset();
    fakeOut_ = false;
    if (!output.empty()) {
      openOutput();
    }
    status = std::to_string(ins_.size()) + " input(s) open" + ((out_ && out_->isPortOpen()) || fakeOut_ ? ", output open" : "");
  } catch (const RtMidiError& e) {
    status = "MIDI error: " + e.getMessage();
  } catch (const std::exception& e) {
    status = std::string("MIDI error: ") + e.what();
  } catch (...) {
    status = "MIDI error";
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
  engine_.setSustainHolds(routing.sustain);
  engine_.setMpeInput(routing.mpe, routing.mpeBendRange);  // K-144
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
      c.surface = x.text("surface");
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
  r.sustain = j.num("sustainHolds", 1) != 0;
  r.sostenuto = j.num("sostenuto", 1) != 0;
  r.soft = j.num("soft", 1) != 0;
  r.softFactor = static_cast<float>(std::clamp(j.num("softFactor", 0.6), 0.1, 1.0));
  r.mpe = j.num("mpe", 0) != 0;  // K-144
  r.mpeBendRange = static_cast<float>(std::clamp(j.num("mpeBendRange", 48), 1.0, 96.0));
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
    if (!c.surface.empty()) x.object["surface"] = Json::makeString(c.surface);
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
  j.object["sustainHolds"] = Json::makeNumber(r.sustain ? 1 : 0);
  j.object["sostenuto"] = Json::makeNumber(r.sostenuto ? 1 : 0);
  j.object["soft"] = Json::makeNumber(r.soft ? 1 : 0);
  j.object["softFactor"] = Json::makeNumber(r.softFactor);
  j.object["mpe"] = Json::makeNumber(r.mpe ? 1 : 0);  // K-144
  j.object["mpeBendRange"] = Json::makeNumber(r.mpeBendRange);
  Json route = Json::makeArray(), bend = Json::makeArray();
  for (int i = 0; i < 16; ++i) {
    route.array.push_back(Json::makeNumber(r.route[i]));
    bend.array.push_back(Json::makeNumber(r.bendRange[i]));
  }
  j.object["route"] = std::move(route);
  j.object["bendRange"] = std::move(bend);
  writeFileAtomic(file, jsonStringify(j) + "\n");
}

// K-142 (0.7.8 follow-up): the input thread wakes the interface for what it takes from here
// (controllers, remote actions, control surfaces, the activity light, a sync start or stop), so a
// hidden Gridline answers at once; the notes themselves went straight to the engine. A clock or time
// code stream (24 or 100 messages a second) wakes it at most ten times a second.
void MidiManager::pokeSync(double t) {
  const double last = syncPokeAt_.load(std::memory_order_relaxed);
  if (last >= 0 && t - last < 0.1) return;
  syncPokeAt_.store(t, std::memory_order_relaxed);
  uiwake::poke();
}

void MidiManager::onMessage(const std::string& port, int portNumber, const std::vector<unsigned char>& msg, bool syncPort) {
  if (msg.empty()) return;
  const double t = nowSec();
  const bool notes = portNumber >= 0;
  if (!notes) portNumber = -portNumber - 2;
  uint8_t s = msg[0], a = msg.size() > 1 ? msg[1] : 0, b = msg.size() > 2 ? msg[2] : 0;
  const auto src = static_cast<SyncSource>(syncSrc_.load());
  if (src == SyncSource::Mtc && syncPort && (s == 0xF1 || s == 0xF0)) {  // MI-02: time code (quarter / full frames)
    bool taken = false;
    {
      std::lock_guard<std::mutex> lk(followerMu_);
      taken = mtc_.message(msg.data(), msg.size(), t);
    }
    if (taken) {  // K-142 (follow-up): a full frame (a locate) at once, quarter frames paced
      if (s == 0xF0) uiwake::poke();
      else pokeSync(t);
      return;
    }
  }
  if (s == 0xF0) {  // sysex: MIDI Machine Control transport; every message is kept for control surfaces
    RemoteAction act = parseMmc(msg);
    std::lock_guard<std::mutex> lk(mu_);
    if (act != RemoteAction::None && actions_.size() < 256) actions_.push_back(act);
    if (sysex_.size() < 64) sysex_.push_back({port, std::vector<uint8_t>(msg.begin(), msg.end()), t});
    uiwake::poke();  // K-142 (follow-up)
    return;
  }
  if (s >= 0xF0) {  // clock, start/continue/stop, song position
    if (src == SyncSource::Clock && syncPort) {
      {
        std::lock_guard<std::mutex> lk(followerMu_);
        follower_.message(s, a, b, t);
      }
      if (s == 0xF8) pokeSync(t);  // K-142 (follow-up): the tempo it implies
      else if (s != 0xFE) uiwake::poke();  // start, continue, stop, song position
    }
    return;
  }
  if (s < 0x80) return;
  std::shared_ptr<SurfacePads> surf;  // a control surface's port: to the UI only, its pads translated
  {
    std::lock_guard<std::mutex> lk(surfMu_);
    if (auto it = surfaces_.find(port); it != surfaces_.end()) surf = it->second;
  }
  if (surf) {
    const int kind = s & 0xF0;
    MidiEvent played;
    if (notes && (s & 0x0F) == surf->channel.load() && (kind == 0x90 || kind == 0x80 || kind == 0xA0)) {
      int16_t& held = surf->held[a & 0x7F];
      int pitch = held, vel = b;
      if (kind == 0x90 && b > 0) {
        pitch = held = surf->pitch[a & 0x7F].load();
        vel = applyVelocityCurve(static_cast<VelocityCurve>(surf->velocity.load()), b, fixedVel_.load());
      } else if (kind != 0xA0) {
        held = -1;
      }
      if (pitch >= 0 && pitch < 128) {
        played = {static_cast<uint8_t>(kind == 0x80 ? 0x90 : kind), static_cast<uint8_t>(pitch),
                  static_cast<uint8_t>(kind == 0x80 ? 0 : vel), port, portNumber, t, true};
        while (engineLock_.test_and_set(std::memory_order_acquire)) {
        }
        engine_.midiEvent(played.status, played.data1, played.data2);
        engineLock_.clear(std::memory_order_release);
      }
    }
    lastActivity_.store(t);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (pending_.size() < 4096) pending_.push_back({s, a, b, port, portNumber, t});
      if (played.status && pending_.size() < 4096) pending_.push_back(played);
    }
    uiwake::poke();  // K-142 (follow-up)
    return;
  }
  const uint32_t deck = packs::platter::hub::deviceId(port);
  packs::platter::hub::push(deck, s, a, b);  // J-150: DJ decks
  const int kind = s & 0xF0;
  bool toEngine = !packs::platter::hub::claimed(deck);  // a deck's controller plays no notes
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
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.size() < 4096) pending_.push_back({s, a, b, port, portNumber, t});
  }
  uiwake::poke();  // K-142 (follow-up)
}

std::vector<MidiEvent> MidiManager::poll() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<MidiEvent> out;
  out.swap(pending_);
  return out;
}

std::vector<SysexEvent> MidiManager::pollSysex() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<SysexEvent> out;
  out.swap(sysex_);
  return out;
}

void MidiManager::claimSurfacePort(const std::string& port, std::shared_ptr<SurfacePads> pads) {
  std::lock_guard<std::mutex> lk(surfMu_);
  surfaces_[port] = std::move(pads);
}

void MidiManager::releaseSurfacePort(const std::string& port) {
  std::lock_guard<std::mutex> lk(surfMu_);
  surfaces_.erase(port);
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
  } catch (...) {
  }
}

std::vector<std::vector<unsigned char>> MidiManager::fakeSent(const std::string& device, bool clear) {
  std::lock_guard<std::mutex> lk(outMu_);
  auto it = fakeSent_.find(device);
  if (it == fakeSent_.end()) return {};
  auto r = it->second;
  if (clear) it->second.clear();
  return r;
}

void MidiManager::sendTo(const std::string& device, const std::vector<unsigned char>& m) {
  if (fake_ && !device.empty() && !m.empty() && midiFakePresent(true, device)) {  // k078-flkey: tests read it back
    std::lock_guard<std::mutex> lk(outMu_);
    auto& v = fakeSent_[device];
    if (v.size() < 20000) v.push_back(m);
    return;
  }
  if (device.empty() || m.empty() || !serviceAvailable() || !systemPorts_ || fake_) {
    return;
  }
  std::lock_guard<std::mutex> lk(outMu_);
  RtMidiOut* o = nullptr;
  for (auto& d : devOuts_)
    if (d.first == device) o = d.second.get();
  if (!o) {
    try {
      auto x = makeMidiOut();
      int pick = -1;
      for (unsigned k = 0; k < x->getPortCount(); ++k) {
        const std::string n = x->getPortName(k);
        if (n == device) {
          pick = static_cast<int>(k);
          break;
        }
        if (pick < 0 && (n.find(device) != std::string::npos || device.find(n) != std::string::npos)) pick = static_cast<int>(k);
      }
      if (pick >= 0) x->openPort(static_cast<unsigned>(pick), "Gridline Deck Out");
      devOuts_.emplace_back(device, std::move(x));
      o = devOuts_.back().second.get();
    } catch (...) {  // RtMidiError or anything else from the driver
      devOuts_.emplace_back(device, nullptr);
      return;
    }
  }
  if (!o || !o->isPortOpen()) return;
  try {
    o->sendMessage(&m);
  } catch (...) {
  }
}

bool MidiManager::outputOpen() {
  std::lock_guard<std::mutex> lk(outMu_);
  return fakeOut_ || (out_ && out_->isPortOpen());
}

bool MidiManager::deviceMessage(const std::string& device, const std::vector<unsigned char>& msg) {
  for (auto& p : ins_) {
    if (p->name == device && p->open()) {
      if (!msg.empty()) {
        deliver(p.get(), msg);
      }
      return true;
    }
  }
  return false;
}

int MidiManager::heldNotes(const std::string& device) const {
  int n = 0;
  for (const auto& p : ins_) {
    if (p->name == device) {
      for (const auto& w : p->held) {
        n += static_cast<int>(std::bitset<64>(w.load(std::memory_order_relaxed)).count());
      }
    }
  }
  return n;
}

int MidiManager::openDeviceOutputs() {
  std::lock_guard<std::mutex> lk(outMu_);
  int n = 0;
  for (const auto& d : devOuts_) {
    if (d.second && d.second->isPortOpen()) {
      ++n;
    }
  }
  return n;
}

// Sends the engine's sync messages when they are heard: the engine stamps them
// with sample positions; the block they belong to started at blockTime() and
// reaches the speakers after the output latency. K-142: the thread sleeps until the
// next message is due, and while there is none until the engine pushes one.
void MidiManager::senderLoop() {
  std::vector<unsigned char> bytes;
  SyncSenderHooks h;
  prepareSyncSender(h);
  h.due = [this](const MidiOutEvent& e) {
    return engine_.blockTime() + static_cast<double>(e.sample - engine_.blockSample()) / engine_.sampleRate() + outputLatency.load() +
           syncOffsetUs_.load() * 1e-6;
  };
  h.now = [] { return nowSec(); };
  h.send = [this, &bytes](const MidiOutEvent& e) {
    bytes.assign(e.data, e.data + e.size);
    sendRaw(bytes);
  };
  h.woke = [] { gl::threads::woke(); };  // K-142: CPU report
  runSyncSender(engine_.syncEvents(), senderWake_, quit_, h);
}

}  // namespace gl

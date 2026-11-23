// Gridline - control surface profiles in the app (Surfaces.h): the device registry, start / stop with
// the handshake, the claimed ports, sysex routing, the throttled idle, Settings rows and dev commands.
#include "Surfaces.h"
#include <chrono>
#include <cstdio>
#include <map>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "App.h"
#include "LaunchpadUi.h"
#include "Midi.h"
#include "Theme.h"

namespace gl::surfaceui {

namespace {

struct Running {
  const Type* type = nullptr;
  std::string port, output;
  std::unique_ptr<Device> dev;
  std::string options;  // the options it runs with
  double lastIdle = 0;
};
std::map<std::string, Running> gRun;  // by port
std::vector<std::string> gOpenWas;    // the open inputs last frame
bool gRescan = false;
// Dev: the bytes sent to devices (surface log on), per port.
bool gLog = false;
std::vector<std::pair<std::string, Bytes>> gSent;

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string hex(const Bytes& b) {
  std::string s;
  char buf[4];
  for (size_t i = 0; i < b.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%02X", b[i]);
    s += (i ? " " : "") + std::string(buf);
  }
  return s;
}

void stop(App& a, Running& r) {
  if (r.dev) r.dev->disconnect();
  if (a.midi) a.midi->releaseSurfacePort(r.port);
  logging::info("midi", r.port + ": " + r.type->label + " back to its standalone mode");
}

void start(App& a, const std::string& port, const Type* t) {
  Running r;
  r.type = t;
  r.port = port;
  r.output = t->output ? t->output(port, a.midi->outputs) : port;
  App* ap = &a;
  const std::string out = r.output, key = port;
  r.dev = t->make(a, port, [ap, out, key](const Bytes& b) {
    if (gLog && gSent.size() < 20000) gSent.push_back({key, b});
    if (ap->midi) ap->midi->sendTo(out, std::vector<unsigned char>(b.begin(), b.end()));
  });
  if (!r.dev) return;
  const MidiInputConfig* c = a.midi->inputConfig(port);
  r.options = c ? c->surface : "";
  r.dev->options(r.options);
  if (auto p = r.dev->pads()) a.midi->claimSurfacePort(port, p);
  r.dev->connect();
  logging::info("midi", port + ": " + t->label + " in DAW mode (answers on " + r.output + ")");
  gRun[port] = std::move(r);
}

}  // namespace

const std::vector<Type>& types() {
  // One line per device profile (each profile's own file makes its Type).
  static const std::vector<Type> t = {launchpad::type()};
  return t;
}

const Type* findType(const std::string& id) {
  for (const Type& t : types())
    if (t.id == id) return &t;
  return nullptr;
}

bool isSurfaceType(const std::string& id) { return findType(id) != nullptr; }

const Type* typeOf(const App& a, const std::string& port) {
  const MidiInputConfig* c = a.midi ? a.midi->inputConfig(port) : nullptr;
  if (c && !c->controllerType.empty()) return findType(c->controllerType);
  for (const Type& t : types())
    if (t.detect && t.detect(port)) return &t;
  return nullptr;
}

void rescanned() { gRescan = true; }

void tick(App& a) {
  if (!a.midi) {
    gRun.clear();
    return;
  }
  const std::vector<std::string> open = a.midi->openInputNames();
  const bool reopened = open != gOpenWas || gRescan;  // a device plugged in, out or reopened: handshakes again
  gOpenWas = open;
  gRescan = false;
  std::map<std::string, const Type*> want;
  for (const auto& port : open) {
    const MidiInputConfig* c = a.midi->inputConfig(port);
    if (c && !c->enabled) continue;
    if (const Type* t = typeOf(a, port)) want[port] = t;
  }
  for (auto it = gRun.begin(); it != gRun.end();) {
    auto w = want.find(it->first);
    if (w == want.end() || w->second != it->second.type) {
      stop(a, it->second);
      it = gRun.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [port, t] : want) {
    auto it = gRun.find(port);
    if (it == gRun.end()) {
      start(a, port, t);
      continue;
    }
    Running& r = it->second;
    const MidiInputConfig* c = a.midi->inputConfig(port);
    if (c && c->surface != r.options) {
      r.options = c->surface;
      r.dev->options(r.options);
    }
    if (reopened) {
      r.output = t->output ? t->output(port, a.midi->outputs) : port;
      r.dev->connect();
    }
  }
  const double now = wallNow();
  for (const auto& ev : a.midi->pollSysex())
    if (auto it = gRun.find(ev.port); it != gRun.end()) it->second.dev->sysex(ev.bytes, now);
  for (auto& [port, r] : gRun) {
    if (now - r.lastIdle < 1.0 / 30.0) continue;
    r.lastIdle = now;
    r.dev->idle(now);
  }
}

bool midi(App& a, const MidiEvent& e) {
  (void)a;
  if (e.surfacePad) return false;  // a pad played as a note: the usual way (recording, step record)
  auto it = gRun.find(e.port);
  if (it == gRun.end()) return false;
  it->second.dev->midi(MidiMessage{e.status, e.data1, e.data2, e.portNumber}, wallNow());
  return true;
}

bool owns(const std::string& port) { return gRun.count(port) != 0; }

void shutdown(App& a) {
  for (auto& [port, r] : gRun) stop(a, r);
  gRun.clear();
}

bool settings(App& a) {
  if (!a.midi || gRun.empty()) return false;
  bool changed = false;
  for (auto& [port, r] : gRun) {
    MidiInputConfig* c = a.midi->inputConfig(port);
    if (!c) continue;
    ImGui::PushID(port.c_str());
    ImGui::SeparatorText((r.type->label + ": " + port).c_str());
    ImGui::TextDisabled("%s", r.dev->status().c_str());
    std::string s = c->surface;
    if (r.dev->settingsUi(s) && s != c->surface) {
      c->surface = s;
      changed = true;
    }
    ImGui::PopID();
  }
  return changed;
}

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  if (cmd != "surface") return false;
  const std::string what = args.empty() ? "list" : args[0];
  if (what == "log") {
    gLog = args.size() < 2 || args[1] != "off";
  } else if (what == "sent") {
    for (const auto& [port, b] : gSent) std::printf("surface frame %d: sent \"%s\" %s\n", frame, port.c_str(), hex(b).c_str());
    if (args.size() > 1 && args[1] == "clear") gSent.clear();
  } else if (what == "clear") {
    gSent.clear();
  } else {
    for (const auto& [port, r] : gRun)
      std::printf("surface frame %d: \"%s\" type %s output \"%s\" %s\n", frame, port.c_str(), r.type->id.c_str(), r.output.c_str(),
                  r.dev->status().c_str());
    if (gRun.empty()) std::printf("surface frame %d: none\n", frame);
  }
  (void)a;
  std::fflush(stdout);
  return true;
}

}  // namespace gl::surfaceui

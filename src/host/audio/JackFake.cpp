// Gridline: in-process fake JACK / PipeWire server (K-357). See JackFake.h.
#include "JackFake.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <tuple>

#include "../../core/ThreadName.h"

namespace gl::jack {

struct FakeServer::FakeClient {
  FakeServer* server = nullptr;
  std::string name;
  bool active = false;
  bool dead = false;
  bool isSystem = false;
  bool isPeer = false;

  ProcessCallback processCb = nullptr;
  void* processArg = nullptr;
  BufferSizeCallback bufCb = nullptr;
  void* bufArg = nullptr;
  SampleRateCallback rateCb = nullptr;
  void* rateArg = nullptr;
  XrunCallback xrunCb = nullptr;
  void* xrunArg = nullptr;
  FreewheelCallback freewheelCb = nullptr;
  void* freewheelArg = nullptr;
  ShutdownCallback shutdownCb = nullptr;
  void* shutdownArg = nullptr;
  InfoShutdownCallback infoShutdownCb = nullptr;
  void* infoShutdownArg = nullptr;
  LatencyCallback latencyCb = nullptr;
  void* latencyArg = nullptr;
  ClientRegistrationCallback clientRegCb = nullptr;
  void* clientRegArg = nullptr;
  PortRegistrationCallback portRegCb = nullptr;
  void* portRegArg = nullptr;
  PortConnectCallback portConnectCb = nullptr;
  void* portConnectArg = nullptr;
  GraphOrderCallback graphOrderCb = nullptr;
  void* graphOrderArg = nullptr;
};

struct FakeServer::FakePort {
  FakeServer* server = nullptr;
  FakeClient* client = nullptr;
  PortId id = 0;
  Uuid uuid = 0;
  std::string shortName;
  std::string fullName;
  std::string type;
  unsigned long flags = 0;
  std::vector<std::string> aliases;
  LatencyRange captureRange{0, 0};
  LatencyRange playbackRange{0, 0};
  std::vector<float> buf;  // pre-allocated to kMaxBufferFrames (never resized in cycle())
  SignalFn peerSignal;
};

namespace {

std::atomic<FakeServer*> gActiveFake{nullptr};

// Packs a list of strings and their NULL-terminated pointer array into one malloc() block so
// jack_free() releases everything cleanly and strings remain valid even if ports mutate later.
const char** packStringList(const std::vector<std::string>& items) {
  if (items.empty()) return nullptr;
  const size_t ptrBytes = (items.size() + 1) * sizeof(const char*);
  size_t strBytes = 0;
  for (const auto& s : items) strBytes += s.size() + 1;
  auto* raw = static_cast<char*>(std::malloc(ptrBytes + strBytes));
  if (!raw) return nullptr;
  auto** table = reinterpret_cast<const char**>(raw);
  char* dst = raw + ptrBytes;
  for (size_t i = 0; i < items.size(); ++i) {
    table[i] = dst;
    std::memcpy(dst, items[i].c_str(), items[i].size() + 1);
    dst += items[i].size() + 1;
  }
  table[items.size()] = nullptr;
  return table;
}

bool patternMatch(const char* pat, const std::string& text) {
  if (!pat || !*pat) return true;
  try {
    return std::regex_search(text, std::regex(pat));
  } catch (...) {
    return text.find(pat) != std::string::npos;
  }
}

FakeServer::FakeClient* asClient(Client* c) { return reinterpret_cast<FakeServer::FakeClient*>(c); }
const FakeServer::FakeClient* asClient(const Client* c) { return reinterpret_cast<const FakeServer::FakeClient*>(c); }
FakeServer::FakePort* asPort(Port* p) { return reinterpret_cast<FakeServer::FakePort*>(p); }
const FakeServer::FakePort* asPort(const Port* p) { return reinterpret_cast<const FakeServer::FakePort*>(p); }

// Static C-ABI trampolines for jack::Api.
Client* fkClientOpen(const char* name, int options, int* status, ...) {
  FakeServer* srv = gActiveFake.load(std::memory_order_acquire);
  if (!srv) {
    if (status) *status = kStatusFailure | kStatusServerFailed;
    return nullptr;
  }
  return srv->openClient(name, options, status);
}
int fkClientClose(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->closeClient(fc) : -1;
}
char* fkGetClientName(Client* c) {
  auto* fc = asClient(c);
  return fc ? fc->name.data() : nullptr;
}
int fkActivate(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->activateClient(fc) : -1;
}
int fkDeactivate(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->deactivateClient(fc) : -1;
}
Nframes fkGetSampleRate(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->sampleRate() : 0;
}
Nframes fkGetBufferSize(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->bufferSize() : 0;
}
int fkSetBufferSize(Client* c, Nframes nframes) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->setBufferSizeImpl(nframes) : -1;
}
int fkSetProcessCb(Client* c, ProcessCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->processCb = cb;
  fc->processArg = arg;
  return 0;
}
int fkSetBufCb(Client* c, BufferSizeCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->bufCb = cb;
  fc->bufArg = arg;
  return 0;
}
int fkSetRateCb(Client* c, SampleRateCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->rateCb = cb;
  fc->rateArg = arg;
  return 0;
}
int fkSetXrunCb(Client* c, XrunCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->xrunCb = cb;
  fc->xrunArg = arg;
  return 0;
}
int fkSetFreewheelCb(Client* c, FreewheelCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->freewheelCb = cb;
  fc->freewheelArg = arg;
  return 0;
}
void fkOnShutdown(Client* c, ShutdownCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return;
  fc->shutdownCb = cb;
  fc->shutdownArg = arg;
}
void fkOnInfoShutdown(Client* c, InfoShutdownCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return;
  fc->infoShutdownCb = cb;
  fc->infoShutdownArg = arg;
}
int fkSetLatencyCb(Client* c, LatencyCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->latencyCb = cb;
  fc->latencyArg = arg;
  return 0;
}
int fkSetClientRegCb(Client* c, ClientRegistrationCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->clientRegCb = cb;
  fc->clientRegArg = arg;
  return 0;
}
int fkSetPortRegCb(Client* c, PortRegistrationCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->portRegCb = cb;
  fc->portRegArg = arg;
  return 0;
}
int fkSetPortConnectCb(Client* c, PortConnectCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->portConnectCb = cb;
  fc->portConnectArg = arg;
  return 0;
}
int fkSetGraphOrderCb(Client* c, GraphOrderCallback cb, void* arg) {
  auto* fc = asClient(c);
  if (!fc) return -1;
  fc->graphOrderCb = cb;
  fc->graphOrderArg = arg;
  return 0;
}
Port* fkPortRegister(Client* c, const char* name, const char* type, unsigned long flags, unsigned long) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->registerPort(fc, name, type, flags) : nullptr;
}
int fkPortUnregister(Client* c, Port* p) {
  auto* fc = asClient(c);
  return fc && fc->server && p ? fc->server->unregisterPort(fc, asPort(p)) : -1;
}
void* fkPortGetBuffer(Port* p, Nframes) {
  auto* fp = asPort(p);
  return fp ? static_cast<void*>(fp->buf.data()) : nullptr;
}
const char* fkPortName(const Port* p) {
  auto* fp = asPort(p);
  return fp ? fp->fullName.c_str() : nullptr;
}
const char* fkPortShortName(const Port* p) {
  auto* fp = asPort(p);
  return fp ? fp->shortName.c_str() : nullptr;
}
int fkPortFlags(const Port* p) {
  auto* fp = asPort(p);
  return fp ? static_cast<int>(fp->flags) : 0;
}
Port* fkPortByName(Client* c, const char* name) {
  auto* fc = asClient(c);
  return fc && fc->server ? reinterpret_cast<Port*>(fc->server->portByNameImpl(name)) : nullptr;
}
const char** fkPortGetAllConnections(const Client* c, const Port* p) {
  auto* fc = asClient(c);
  return fc && fc->server && p ? fc->server->getAllConnections(asPort(p)) : nullptr;
}
const char** fkGetPorts(Client* c, const char* pat, const char* typePat, unsigned long flags) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->getPorts(pat, typePat, flags) : nullptr;
}
int fkConnect(Client* c, const char* src, const char* dst) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->connectImpl(src, dst) : -1;
}
int fkDisconnect(Client* c, const char* src, const char* dst) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->disconnectImpl(src, dst) : -1;
}
void fkFree(void* p) { std::free(p); }
int fkPortSetAlias(Port* p, const char* alias) {
  auto* fp = asPort(p);
  return fp && fp->server ? fp->server->setAliasImpl(fp, alias) : -1;
}
int fkPortUnsetAlias(Port* p, const char* alias) {
  auto* fp = asPort(p);
  return fp && fp->server ? fp->server->unsetAliasImpl(fp, alias) : -1;
}
void fkPortGetLatencyRange(Port* p, int mode, LatencyRange* range) {
  auto* fp = asPort(p);
  if (!range) return;
  if (!fp) {
    *range = {0, 0};
    return;
  }
  *range = (mode == kCaptureLatency) ? fp->captureRange : fp->playbackRange;
}
void fkPortSetLatencyRange(Port* p, int mode, LatencyRange* range) {
  auto* fp = asPort(p);
  if (!fp || !range) return;
  if (mode == kCaptureLatency)
    fp->captureRange = *range;
  else
    fp->playbackRange = *range;
}
int fkRecomputeLatencies(Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->recomputeLatenciesImpl() : -1;
}
Uuid fkPortUuid(const Port* p) {
  auto* fp = asPort(p);
  return fp ? fp->uuid : 0;
}
int fkSetProperty(Client* c, Uuid subject, const char* key, const char* value, const char*) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->setPropertyImpl(subject, key, value) : -1;
}
int fkRemoveProperty(Client* c, Uuid subject, const char* key) {
  auto* fc = asClient(c);
  return fc && fc->server ? fc->server->removePropertyImpl(subject, key) : -1;
}
const char* fkVersionString() { return "PipeWire-Fake-JACK 1.0"; }
Nframes fkFrameTime(const Client* c) {
  auto* fc = asClient(c);
  return fc && fc->server ? static_cast<Nframes>(fc->server->cycleCount() * fc->server->bufferSize()) : 0;
}

}  // namespace

FakeServer::FakeServer() : FakeServer(Spec{}) {}

FakeServer::FakeServer(Spec spec) : spec_(spec) {
  rate_.store(spec_.sampleRate > 0 ? spec_.sampleRate : 48000);
  bufSize_.store(std::clamp<Nframes>(spec_.bufferSize > 0 ? spec_.bufferSize : 256, 16, kMaxBufferFrames));
  if (spec_.addDefaultPhysical) {
    std::lock_guard<std::mutex> lock(mu_);
    createPhysicalPortsLocked();
  }
  makeActive();
}

FakeServer::~FakeServer() {
  stopClock();
  FakeServer* expected = this;
  gActiveFake.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void FakeServer::makeActive() { gActiveFake.store(this, std::memory_order_release); }

Api FakeServer::api() {
  makeActive();
  Api a;
  a.client_open = &fkClientOpen;
  a.client_close = &fkClientClose;
  a.get_client_name = &fkGetClientName;
  a.activate = &fkActivate;
  a.deactivate = &fkDeactivate;
  a.get_sample_rate = &fkGetSampleRate;
  a.get_buffer_size = &fkGetBufferSize;
  a.set_process_callback = &fkSetProcessCb;
  a.set_buffer_size_callback = &fkSetBufCb;
  a.set_sample_rate_callback = &fkSetRateCb;
  a.set_xrun_callback = &fkSetXrunCb;
  a.on_shutdown = &fkOnShutdown;
  a.port_register = &fkPortRegister;
  a.port_unregister = &fkPortUnregister;
  a.port_get_buffer = &fkPortGetBuffer;
  a.port_name = &fkPortName;
  a.port_short_name = &fkPortShortName;
  a.port_flags = &fkPortFlags;
  a.port_by_name = &fkPortByName;
  a.port_get_all_connections = &fkPortGetAllConnections;
  a.get_ports = &fkGetPorts;
  a.connect = &fkConnect;
  a.disconnect = &fkDisconnect;
  a.free = &fkFree;

  a.set_buffer_size = &fkSetBufferSize;
  a.set_freewheel_callback = &fkSetFreewheelCb;
  a.on_info_shutdown = &fkOnInfoShutdown;
  a.set_latency_callback = &fkSetLatencyCb;
  a.set_client_registration_callback = &fkSetClientRegCb;
  a.set_port_registration_callback = &fkSetPortRegCb;
  a.set_port_connect_callback = &fkSetPortConnectCb;
  a.set_graph_order_callback = &fkSetGraphOrderCb;
  a.port_set_alias = &fkPortSetAlias;
  a.port_unset_alias = &fkPortUnsetAlias;
  a.port_get_latency_range = &fkPortGetLatencyRange;
  a.port_set_latency_range = &fkPortSetLatencyRange;
  a.recompute_total_latencies = &fkRecomputeLatencies;
  a.port_uuid = &fkPortUuid;
  a.set_property = &fkSetProperty;
  a.remove_property = &fkRemoveProperty;
  a.get_version_string = &fkVersionString;
  a.frame_time = &fkFrameTime;

  a.libraryPath = "fake://pipewire-jack";
  a.viaPipeWire = spec_.viaPipeWire;
  a.fake = true;
  return a;
}

void FakeServer::createPhysicalPortsLocked() {
  auto sys = std::make_unique<FakeClient>();
  sys->server = this;
  sys->name = "system";
  sys->active = true;
  sys->isSystem = true;
  FakeClient* sc = sys.get();
  clients_.push_back(std::move(sys));

  auto add = [&](const std::string& shortName, unsigned long flags, Nframes cap, Nframes play) {
    auto p = std::make_unique<FakePort>();
    p->server = this;
    p->client = sc;
    p->id = nextPortId_++;
    p->uuid = nextUuid_++;
    p->shortName = shortName;
    p->fullName = "system:" + shortName;
    p->type = kDefaultAudioType;
    p->flags = flags | kPortIsPhysical | kPortIsTerminal;
    p->captureRange = {cap, cap};
    p->playbackRange = {play, play};
    p->buf.assign(kMaxBufferFrames, 0.f);
    ports_.push_back(std::move(p));
  };
  add("capture_1", kPortIsOutput, spec_.captureLatency, 0);
  add("capture_2", kPortIsOutput, spec_.captureLatency, 0);
  add("playback_1", kPortIsInput, 0, spec_.playbackLatency);
  add("playback_2", kPortIsInput, 0, spec_.playbackLatency);
}

void FakeServer::setServerRunning(bool on) { serverRunning_.store(on, std::memory_order_release); }

std::string FakeServer::uniqueClientNameLocked(const std::string& requested, bool* notUnique) const {
  auto taken = [&](const std::string& cand) {
    for (const auto& c : clients_)
      if (c->name == cand) return true;
    return false;
  };
  if (!taken(requested)) {
    if (notUnique) *notUnique = false;
    return requested;
  }
  if (notUnique) *notUnique = true;
  for (int i = 1; i < 100; ++i) {
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "-%02d", i);
    const std::string cand = requested + suffix;
    if (!taken(cand)) return cand;
  }
  return requested + "-99";
}

Client* FakeServer::openClient(const char* name, int options, int* status) {
  if (!serverRunning_.load(std::memory_order_acquire)) {
    if (status) *status = kStatusFailure | kStatusServerFailed;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mu_);
  const std::string req = (name && *name) ? std::string(name) : std::string("Gridline");
  bool collided = false;
  const std::string assigned = uniqueClientNameLocked(req, &collided);
  if ((options & kUseExactName) && collided) {
    if (status) *status = kStatusFailure | kStatusNameNotUnique;
    return nullptr;
  }
  if (status) *status = collided ? kStatusNameNotUnique : 0;
  auto c = std::make_unique<FakeClient>();
  c->server = this;
  c->name = assigned;
  FakeClient* ptr = c.get();
  clients_.push_back(std::move(c));
  return reinterpret_cast<Client*>(ptr);
}

int FakeServer::closeClient(FakeClient* c) {
  if (!c) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  // Remove edges and ports owned by this client.
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                              [c](const auto& e) { return e.first->client == c || e.second->client == c; }),
               edges_.end());
  for (const auto& p : ports_) {
    if (p->client == c) props_.erase(p->uuid);
  }
  ports_.erase(std::remove_if(ports_.begin(), ports_.end(), [c](const auto& p) { return p->client == c; }), ports_.end());
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [c](const auto& x) { return x.get() == c; }), clients_.end());
  return 0;
}

int FakeServer::activateClient(FakeClient* c) {
  if (!c || c->dead) return -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    c->active = true;
    recomputeLatenciesLocked();
  }
  if (spec_.threaded) startClock();
  return 0;
}

int FakeServer::deactivateClient(FakeClient* c) {
  if (!c) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  c->active = false;
  // Real JACK drops connections of a deactivated client's ports.
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                              [c](const auto& e) { return e.first->client == c || e.second->client == c; }),
               edges_.end());
  return 0;
}

Port* FakeServer::registerPort(FakeClient* c, const char* shortName, const char* type, unsigned long flags) {
  if (!c || c->dead || !shortName || !*shortName) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  const std::string full = c->name + ":" + shortName;
  if (findPortLocked(full)) return nullptr;
  auto p = std::make_unique<FakePort>();
  p->server = this;
  p->client = c;
  p->id = nextPortId_++;
  p->uuid = nextUuid_++;
  p->shortName = shortName;
  p->fullName = full;
  p->type = (type && *type) ? type : kDefaultAudioType;
  p->flags = flags;
  p->buf.assign(kMaxBufferFrames, 0.f);
  FakePort* ptr = p.get();
  ports_.push_back(std::move(p));
  return reinterpret_cast<Port*>(ptr);
}

int FakeServer::unregisterPort(FakeClient* c, FakePort* p) {
  if (!c || !p || p->client != c) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [p](const auto& e) { return e.first == p || e.second == p; }),
               edges_.end());
  props_.erase(p->uuid);
  ports_.erase(std::remove_if(ports_.begin(), ports_.end(), [p](const auto& x) { return x.get() == p; }), ports_.end());
  return 0;
}

FakeServer::FakePort* FakeServer::findPortLocked(const std::string& fullName) const {
  for (const auto& p : ports_) {
    if (p->fullName == fullName) return p.get();
    for (const auto& al : p->aliases)
      if (al == fullName) return p.get();
  }
  return nullptr;
}

FakeServer::FakePort* FakeServer::findPortByUuidLocked(Uuid id) const {
  for (const auto& p : ports_)
    if (p->uuid == id) return p.get();
  return nullptr;
}

FakeServer::FakePort* FakeServer::portByNameImpl(const char* fullName) const {
  if (!fullName || !*fullName) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  return findPortLocked(fullName);
}

const char** FakeServer::getAllConnections(const FakePort* p) const {
  if (!p) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> peers;
  for (const auto& e : edges_) {
    if (e.first == p) peers.push_back(e.second->fullName);
    else if (e.second == p) peers.push_back(e.first->fullName);
  }
  return packStringList(peers);
}

const char** FakeServer::getPorts(const char* namePattern, const char* typePattern, unsigned long flags) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  for (const auto& p : ports_) {
    if (flags != 0 && (p->flags & flags) != flags) continue;
    if (!patternMatch(namePattern, p->fullName)) continue;
    if (!patternMatch(typePattern, p->type)) continue;
    out.push_back(p->fullName);
  }
  return packStringList(out);
}

int FakeServer::connectImpl(const char* src, const char* dst) {
  if (!src || !dst) return -1;
  std::vector<std::tuple<PortConnectCallback, void*, PortId, PortId>> connectCbs;
  std::vector<std::pair<GraphOrderCallback, void*>> graphCbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    FakePort* s = findPortLocked(src);
    FakePort* d = findPortLocked(dst);
    if (!s || !d) return -1;
    if (!(s->flags & kPortIsOutput) || !(d->flags & kPortIsInput)) return -1;
    for (const auto& e : edges_)
      if (e.first == s && e.second == d) return 17;  // EEXIST (same convention as libjack)
    edges_.emplace_back(s, d);
    recomputeLatenciesLocked();
    for (const auto& c : clients_) {
      if (c->active && !c->dead) {
        if (c->portConnectCb) connectCbs.emplace_back(c->portConnectCb, c->portConnectArg, s->id, d->id);
        if (c->graphOrderCb) graphCbs.emplace_back(c->graphOrderCb, c->graphOrderArg);
      }
    }
  }
  for (const auto& [cb, arg, a, b] : connectCbs) cb(a, b, 1, arg);
  for (const auto& [cb, arg] : graphCbs) cb(arg);
  return 0;
}

int FakeServer::disconnectImpl(const char* src, const char* dst) {
  if (!src || !dst) return -1;
  std::vector<std::tuple<PortConnectCallback, void*, PortId, PortId>> connectCbs;
  std::vector<std::pair<GraphOrderCallback, void*>> graphCbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    FakePort* s = findPortLocked(src);
    FakePort* d = findPortLocked(dst);
    if (!s || !d) return -1;
    const auto it = std::find(edges_.begin(), edges_.end(), std::make_pair(s, d));
    if (it == edges_.end()) return -1;
    edges_.erase(it);
    recomputeLatenciesLocked();
    for (const auto& c : clients_) {
      if (c->active && !c->dead) {
        if (c->portConnectCb) connectCbs.emplace_back(c->portConnectCb, c->portConnectArg, s->id, d->id);
        if (c->graphOrderCb) graphCbs.emplace_back(c->graphOrderCb, c->graphOrderArg);
      }
    }
  }
  for (const auto& [cb, arg, a, b] : connectCbs) cb(a, b, 0, arg);
  for (const auto& [cb, arg] : graphCbs) cb(arg);
  return 0;
}

int FakeServer::setBufferSizeImpl(Nframes nframes) {
  if (nframes < 16 || nframes > kMaxBufferFrames) return -1;
  injectBufferSizeChange(nframes);
  return 0;
}

void FakeServer::recomputeLatenciesLocked() {
  // Propagate capture latencies from upstream sources to non-physical inputs,
  // and playback latencies from downstream sinks to non-physical outputs.
  for (auto& p : ports_) {
    if (p->flags & kPortIsPhysical) continue;
    if (p->flags & kPortIsInput) {
      LatencyRange r{0, 0};
      bool any = false;
      for (const auto& e : edges_) {
        if (e.second == p.get()) {
          if (!any) {
            r = e.first->captureRange;
            any = true;
          } else {
            r.min = std::min(r.min, e.first->captureRange.min);
            r.max = std::max(r.max, e.first->captureRange.max);
          }
        }
      }
      p->captureRange = r;
    } else if (p->flags & kPortIsOutput) {
      LatencyRange r{0, 0};
      bool any = false;
      for (const auto& e : edges_) {
        if (e.first == p.get()) {
          if (!any) {
            r = e.second->playbackRange;
            any = true;
          } else {
            r.min = std::min(r.min, e.second->playbackRange.min);
            r.max = std::max(r.max, e.second->playbackRange.max);
          }
        }
      }
      p->playbackRange = r;
    }
  }
  // Invoke client latency callbacks (note: lock held in fake is safe because Host::onLatency only
  // calls port_get_latency_range / port_set_latency_range which do not take mu_).
  for (const auto& c : clients_) {
    if (c->active && !c->dead && c->latencyCb) {
      c->latencyCb(kCaptureLatency, c->latencyArg);
      c->latencyCb(kPlaybackLatency, c->latencyArg);
    }
  }
}

int FakeServer::recomputeLatenciesImpl() {
  std::lock_guard<std::mutex> lock(mu_);
  recomputeLatenciesLocked();
  return 0;
}

int FakeServer::setAliasImpl(FakePort* p, const char* alias) {
  if (!p || !alias || !*alias) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  if (std::find(p->aliases.begin(), p->aliases.end(), alias) == p->aliases.end()) p->aliases.emplace_back(alias);
  return 0;
}

int FakeServer::unsetAliasImpl(FakePort* p, const char* alias) {
  if (!p || !alias) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  p->aliases.erase(std::remove(p->aliases.begin(), p->aliases.end(), std::string(alias)), p->aliases.end());
  return 0;
}

int FakeServer::setPropertyImpl(Uuid subject, const char* key, const char* value) {
  if (!key || !value) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  if (!findPortByUuidLocked(subject)) return -1;
  props_[subject][key] = value;
  return 0;
}

int FakeServer::removePropertyImpl(Uuid subject, const char* key) {
  if (!key) return -1;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = props_.find(subject);
  if (it != props_.end()) it->second.erase(key);
  return 0;
}

void FakeServer::cycle(Nframes frames) {
  if (!serverRunning_.load(std::memory_order_acquire)) return;
  const Nframes n = std::clamp<Nframes>(frames > 0 ? frames : bufSize_.load(std::memory_order_relaxed), 1, kMaxBufferFrames);
  const uint64_t pos = framePos_.fetch_add(n, std::memory_order_relaxed);

  // Zero heap allocations inside cycle(): iterate pre-allocated containers in place under mu_.
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& p : ports_) {
    std::memset(p->buf.data(), 0, sizeof(float) * static_cast<size_t>(n));
  }
  // 1. Peer output signals (e.g. "Tone:out_1") write into their output buffers.
  for (auto& p : ports_) {
    if ((p->flags & kPortIsOutput) && p->peerSignal) {
      p->peerSignal(p->buf.data(), n, pos);
    }
  }
  // 2. Mix peer/physical output buffers into connected client input buffers.
  for (const auto& e : edges_) {
    if (e.first->client && (e.first->client->isPeer || e.first->client->isSystem)) {
      const float* src = e.first->buf.data();
      float* dst = e.second->buf.data();
      for (Nframes i = 0; i < n; ++i) dst[i] += src[i];
    }
  }
  // 3. Run every active client's realtime process callback.
  for (const auto& c : clients_) {
    if (c->active && !c->dead && c->processCb) {
      c->processCb(n, c->processArg);
    }
  }
  // 4. Mix active client output buffers into connected downstream input ports (speakers, peers).
  for (const auto& e : edges_) {
    if (e.first->client && !e.first->client->isPeer && !e.first->client->isSystem) {
      const float* src = e.first->buf.data();
      float* dst = e.second->buf.data();
      for (Nframes i = 0; i < n; ++i) dst[i] += src[i];
    }
  }
  cycleCount_.fetch_add(1, std::memory_order_relaxed);
}

void FakeServer::startClock() {
  if (clockRunning_.exchange(true)) return;
  clockThread_ = std::thread([this]() {
    gl::threads::setName("gl-jack-fake");
    runClock();
  });
}

void FakeServer::stopClock() {
  if (!clockRunning_.exchange(false)) return;
  if (clockThread_.joinable()) clockThread_.join();
}

void FakeServer::runClock() {
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  while (clockRunning_.load(std::memory_order_acquire)) {
    const Nframes n = bufSize_.load(std::memory_order_relaxed);
    const Nframes r = std::max<Nframes>(1, rate_.load(std::memory_order_relaxed));
    cycle(n);
    next += std::chrono::nanoseconds(static_cast<long long>(1e9 * static_cast<double>(n) / static_cast<double>(r)));
    const auto now = clock::now();
    if (next < now - std::chrono::milliseconds(200)) next = now;
    std::this_thread::sleep_until(next);
  }
}

void FakeServer::addPeerClient(const std::string& clientName, const std::vector<std::string>& outPorts,
                               const std::vector<std::string>& inPorts) {
  std::vector<std::pair<ClientRegistrationCallback, void*>> clientCbs;
  std::vector<std::tuple<PortRegistrationCallback, void*, PortId>> portCbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Replace if already present.
    for (const auto& c : clients_)
      if (c->name == clientName) return;
    auto c = std::make_unique<FakeClient>();
    c->server = this;
    c->name = clientName;
    c->active = true;
    c->isPeer = true;
    FakeClient* ptr = c.get();
    clients_.push_back(std::move(c));

    std::vector<PortId> addedIds;
    auto addP = [&](const std::string& sname, unsigned long flags) {
      auto p = std::make_unique<FakePort>();
      p->server = this;
      p->client = ptr;
      p->id = nextPortId_++;
      p->uuid = nextUuid_++;
      p->shortName = sname;
      p->fullName = clientName + ":" + sname;
      p->type = kDefaultAudioType;
      p->flags = flags;
      p->buf.assign(kMaxBufferFrames, 0.f);
      addedIds.push_back(p->id);
      ports_.push_back(std::move(p));
    };
    for (const auto& n : outPorts) addP(n, kPortIsOutput);
    for (const auto& n : inPorts) addP(n, kPortIsInput);

    for (const auto& cl : clients_) {
      if (cl->active && !cl->dead && !cl->isPeer && !cl->isSystem) {
        if (cl->clientRegCb) clientCbs.emplace_back(cl->clientRegCb, cl->clientRegArg);
        if (cl->portRegCb)
          for (PortId pid : addedIds) portCbs.emplace_back(cl->portRegCb, cl->portRegArg, pid);
      }
    }
  }
  for (const auto& [cb, arg] : clientCbs) cb(clientName.c_str(), 1, arg);
  for (const auto& [cb, arg, pid] : portCbs) cb(pid, 1, arg);
}

void FakeServer::removePeerClient(const std::string& clientName) {
  std::vector<std::pair<ClientRegistrationCallback, void*>> clientCbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    FakeClient* target = nullptr;
    for (const auto& c : clients_)
      if (c->name == clientName && c->isPeer) target = c.get();
    if (!target) return;
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [target](const auto& e) { return e.first->client == target || e.second->client == target; }),
                 edges_.end());
    for (const auto& p : ports_)
      if (p->client == target) props_.erase(p->uuid);
    ports_.erase(std::remove_if(ports_.begin(), ports_.end(), [target](const auto& p) { return p->client == target; }),
                 ports_.end());
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [target](const auto& x) { return x.get() == target; }),
                   clients_.end());
    for (const auto& cl : clients_) {
      if (cl->active && !cl->dead && !cl->isPeer && !cl->isSystem && cl->clientRegCb)
        clientCbs.emplace_back(cl->clientRegCb, cl->clientRegArg);
    }
  }
  for (const auto& [cb, arg] : clientCbs) cb(clientName.c_str(), 0, arg);
}

void FakeServer::setPeerSignal(const std::string& fullPortName, SignalFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  if (FakePort* p = findPortLocked(fullPortName)) p->peerSignal = std::move(fn);
}

void FakeServer::setPhysicalLatencies(Nframes capture, Nframes playback) {
  std::lock_guard<std::mutex> lock(mu_);
  spec_.captureLatency = capture;
  spec_.playbackLatency = playback;
  for (auto& p : ports_) {
    if (!(p->flags & kPortIsPhysical)) continue;
    if (p->flags & kPortIsOutput) p->captureRange = {capture, capture};
    if (p->flags & kPortIsInput) p->playbackRange = {playback, playback};
  }
  recomputeLatenciesLocked();
}

bool FakeServer::connectPorts(const std::string& srcFull, const std::string& dstFull) {
  const int r = connectImpl(srcFull.c_str(), dstFull.c_str());
  return r == 0 || r == 17;
}

bool FakeServer::disconnectPorts(const std::string& srcFull, const std::string& dstFull) {
  return disconnectImpl(srcFull.c_str(), dstFull.c_str()) == 0;
}

bool FakeServer::isConnected(const std::string& srcFull, const std::string& dstFull) const {
  std::lock_guard<std::mutex> lock(mu_);
  const FakePort* s = findPortLocked(srcFull);
  const FakePort* d = findPortLocked(dstFull);
  if (!s || !d) return false;
  return std::find(edges_.begin(), edges_.end(), std::make_pair(const_cast<FakePort*>(s), const_cast<FakePort*>(d))) !=
         edges_.end();
}

void FakeServer::injectShutdown(int code, const std::string& reason) {
  struct Target {
    InfoShutdownCallback infoCb = nullptr;
    void* infoArg = nullptr;
    ShutdownCallback cb = nullptr;
    void* arg = nullptr;
  };
  std::vector<Target> targets;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& c : clients_) {
      if (c->isSystem || c->isPeer) continue;
      c->active = false;
      c->dead = true;
      targets.push_back({c->infoShutdownCb, c->infoShutdownArg, c->shutdownCb, c->shutdownArg});
    }
  }
  for (const auto& t : targets) {
    if (t.infoCb) t.infoCb(code, reason.c_str(), t.infoArg);
    else if (t.cb) t.cb(t.arg);
  }
}

void FakeServer::injectRateChange(Nframes newRate) {
  if (newRate == 0) return;
  rate_.store(newRate, std::memory_order_release);
  std::vector<std::pair<SampleRateCallback, void*>> cbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& c : clients_)
      if (c->active && !c->dead && c->rateCb) cbs.emplace_back(c->rateCb, c->rateArg);
  }
  for (const auto& [cb, arg] : cbs) cb(newRate, arg);
}

void FakeServer::injectBufferSizeChange(Nframes newBufferSize) {
  const Nframes clamped = std::clamp<Nframes>(newBufferSize, 16, kMaxBufferFrames);
  bufSize_.store(clamped, std::memory_order_release);
  std::vector<std::pair<BufferSizeCallback, void*>> cbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& c : clients_)
      if (c->active && !c->dead && c->bufCb) cbs.emplace_back(c->bufCb, c->bufArg);
  }
  for (const auto& [cb, arg] : cbs) cb(clamped, arg);
}

void FakeServer::injectXrun() {
  std::vector<std::pair<XrunCallback, void*>> cbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& c : clients_)
      if (c->active && !c->dead && c->xrunCb) cbs.emplace_back(c->xrunCb, c->xrunArg);
  }
  for (const auto& [cb, arg] : cbs) cb(arg);
}

void FakeServer::injectFreewheel(bool starting) {
  std::vector<std::pair<FreewheelCallback, void*>> cbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& c : clients_)
      if (c->active && !c->dead && c->freewheelCb) cbs.emplace_back(c->freewheelCb, c->freewheelArg);
  }
  for (const auto& [cb, arg] : cbs) cb(starting ? 1 : 0, arg);
}

void FakeServer::injectLatencyChange() { recomputeLatenciesImpl(); }

std::vector<std::string> FakeServer::clientNames() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  for (const auto& c : clients_) out.push_back(c->name);
  return out;
}

std::vector<std::string> FakeServer::portNames(const std::string& clientPrefix) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  for (const auto& p : ports_) {
    if (clientPrefix.empty() || p->fullName.rfind(clientPrefix, 0) == 0) out.push_back(p->fullName);
  }
  return out;
}

std::vector<std::string> FakeServer::portAliases(const std::string& fullPortName) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (const FakePort* p = findPortLocked(fullPortName)) return p->aliases;
  return {};
}

std::string FakeServer::portProperty(const std::string& fullPortName, const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  const FakePort* p = findPortLocked(fullPortName);
  if (!p) return {};
  auto it = props_.find(p->uuid);
  if (it == props_.end()) return {};
  auto kv = it->second.find(key);
  return kv == it->second.end() ? std::string() : kv->second;
}

LatencyRange FakeServer::portLatency(const std::string& fullPortName, int mode) const {
  std::lock_guard<std::mutex> lock(mu_);
  const FakePort* p = findPortLocked(fullPortName);
  if (!p) return {0, 0};
  return mode == kCaptureLatency ? p->captureRange : p->playbackRange;
}

std::vector<float> FakeServer::lastBuffer(const std::string& fullPortName, Nframes frames) const {
  std::lock_guard<std::mutex> lock(mu_);
  const FakePort* p = findPortLocked(fullPortName);
  if (!p) return {};
  const size_t n = std::min<size_t>(frames, p->buf.size());
  return std::vector<float>(p->buf.begin(), p->buf.begin() + static_cast<std::ptrdiff_t>(n));
}

FakeServer& appFakeServer() {
  static std::unique_ptr<FakeServer> inst = []() {
    FakeServer::Spec s;
    s.sampleRate = 48000;
    s.bufferSize = 256;
    s.captureLatency = 128;
    s.playbackLatency = 256;
    s.viaPipeWire = true;
    s.addDefaultPhysical = true;
    s.threaded = true;
    auto srv = std::make_unique<FakeServer>(s);
    srv->addPeerClient("Tone", {"out_1", "out_2"});
    srv->setPeerSignal("Tone:out_1", [](float* out, Nframes n, uint64_t pos) {
      constexpr double kTwoPi = 6.28318530717958647692;
      for (Nframes i = 0; i < n; ++i)
        out[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * 220.0 * static_cast<double>(pos + i) / 48000.0));
    });
    srv->setPeerSignal("Tone:out_2", [](float* out, Nframes n, uint64_t pos) {
      constexpr double kTwoPi = 6.28318530717958647692;
      for (Nframes i = 0; i < n; ++i)
        out[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * 330.0 * static_cast<double>(pos + i) / 48000.0));
    });
    return srv;
  }();
  return *inst;
}

}  // namespace gl::jack

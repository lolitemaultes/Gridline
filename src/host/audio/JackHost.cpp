// Gridline: JACK / PipeWire audio host (K-357). See JackHost.h.
#include "JackHost.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../core/ThreadName.h"

namespace gl::jack {

namespace {

std::vector<std::string> takeJackStringList(const Api& api, const char** list) {
  std::vector<std::string> out;
  if (!list) return out;
  for (const char** p = list; *p; ++p) out.emplace_back(*p);
  api.free(static_cast<void*>(const_cast<char**>(list)));
  return out;
}

std::string openStatusMessage(int status) {
  if (status & kStatusServerFailed) return "No JACK or PipeWire server is running.";
  if (status & kStatusVersionError) return "JACK protocol version mismatch.";
  if (status & kStatusShmFailure) return "JACK shared memory error.";
  if (status & kStatusInitFailure) return "JACK client initialization failed.";
  return "Could not connect to a JACK or PipeWire server.";
}

}  // namespace

bool Host::open(const Api& api, const Config& cfg, audio::RenderFn fn, void* user, std::string* err) {
  close();
  auto fail = [&](const std::string& msg) {
    if (err) *err = msg;
    close();
    return false;
  };
  if (!apiComplete(api)) return fail("JACK API table is incomplete.");

  api_ = &api;
  fn_ = fn;
  user_ = user;
  serverShutDown_ = false;
  reasonReady_.store(false, std::memory_order_relaxed);
  shutdownReason_[0] = '\0';

#if !defined(_WIN32)
  // PipeWire quantum request: only write PIPEWIRE_LATENCY if explicitly requested and not set by user.
  bool restorePwEnv = false;
  if (cfg.requestFrames > 0 && api.viaPipeWire && !std::getenv("PIPEWIRE_LATENCY")) {
    const std::string val = pipewireLatencyEnv(cfg.requestFrames, 48000);
    if (!val.empty()) {
      setenv("PIPEWIRE_LATENCY", val.c_str(), 0);
      restorePwEnv = true;
    }
  }
  if (api.viaPipeWire && !std::getenv("PIPEWIRE_PROPS")) {
    setenv("PIPEWIRE_PROPS", "media.category=Duplex node.description=Gridline", 0);
  }
#endif

  const std::string reqName = sanitizeClientName(cfg.clientName);
  int status = 0;
  client_ = api_->client_open(reqName.c_str(), kNoStartServer, &status);

#if !defined(_WIN32)
  if (restorePwEnv) unsetenv("PIPEWIRE_LATENCY");
#endif

  if (!client_) {
    api_ = nullptr;
    fn_ = nullptr;
    user_ = nullptr;
    return fail(openStatusMessage(status));
  }

  const char* actualName = api_->get_client_name(client_);
  clientName_ = (actualName && *actualName) ? actualName : reqName;

  // Apply explicit buffer size on JACK2 when requested (on PipeWire, PIPEWIRE_LATENCY above handles it).
  if (cfg.requestFrames > 0 && !api_->viaPipeWire && api_->set_buffer_size) {
    api_->set_buffer_size(client_, static_cast<Nframes>(cfg.requestFrames));
  }

  rate_ = static_cast<int>(api_->get_sample_rate(client_));
  bufferFrames_ = static_cast<int>(api_->get_buffer_size(client_));
  if (rate_ <= 0) rate_ = 48000;
  if (bufferFrames_ <= 0) bufferFrames_ = 256;
  pendingRate_.store(rate_, std::memory_order_relaxed);
  pendingBuffer_.store(bufferFrames_, std::memory_order_relaxed);

  // Register callbacks before port registration and activation.
  if (api_->set_process_callback(client_, &Host::onProcess, this) != 0)
    return fail("Could not register JACK process callback.");
  api_->set_buffer_size_callback(client_, &Host::onBufferSize, this);
  api_->set_sample_rate_callback(client_, &Host::onSampleRate, this);
  api_->set_xrun_callback(client_, &Host::onXrun, this);
  if (api_->set_freewheel_callback) api_->set_freewheel_callback(client_, &Host::onFreewheel, this);
  if (api_->on_info_shutdown)
    api_->on_info_shutdown(client_, &Host::onInfoShutdown, this);
  else
    api_->on_shutdown(client_, &Host::onShutdown, this);
  if (api_->set_latency_callback) api_->set_latency_callback(client_, &Host::onLatency, this);
  if (api_->set_client_registration_callback) api_->set_client_registration_callback(client_, &Host::onClientRegistration, this);
  if (api_->set_port_registration_callback) api_->set_port_registration_callback(client_, &Host::onPortRegistration, this);
  if (api_->set_port_connect_callback) api_->set_port_connect_callback(client_, &Host::onPortConnect, this);
  if (api_->set_graph_order_callback) api_->set_graph_order_callback(client_, &Host::onGraphOrder, this);

  // Register planned fixed pool of named audio input and output ports.
  plan_ = planPorts(cfg.inputs, cfg.extraPairs);
  inPort_.reserve(plan_.ins.size());
  inAlias_.assign(plan_.ins.size(), std::string());
  for (size_t i = 0; i < plan_.ins.size(); ++i) {
    Port* p = api_->port_register(client_, plan_.ins[i].name.c_str(), kDefaultAudioType, kPortIsInput, 0);
    if (!p) return fail("Could not register JACK input port \"" + plan_.ins[i].name + "\".");
    inPort_.push_back(p);
    applyAliasAndMeta(p, inAlias_[i], plan_.ins[i].alias);
  }
  outPort_.reserve(plan_.outs.size());
  outAlias_.assign(plan_.outs.size(), std::string());
  for (size_t i = 0; i < plan_.outs.size(); ++i) {
    Port* p = api_->port_register(client_, plan_.outs[i].name.c_str(), kDefaultAudioType, kPortIsOutput, 0);
    if (!p) return fail("Could not register JACK output port \"" + plan_.outs[i].name + "\".");
    outPort_.push_back(p);
    applyAliasAndMeta(p, outAlias_[i], plan_.outs[i].alias);
  }

  events_.store(0, std::memory_order_relaxed);
  xruns_.store(0, std::memory_order_relaxed);
  cycles_.store(0, std::memory_order_relaxed);
  freewheel_.store(false, std::memory_order_relaxed);
  return true;
}

bool Host::activate(std::string* err) {
  if (!client_ || !api_) {
    if (err) *err = "No JACK client is open.";
    return false;
  }
  if (active_) return true;
  events_.store(0, std::memory_order_relaxed);
  live_.store(true, std::memory_order_release);
  if (api_->activate(client_) != 0) {
    live_.store(false, std::memory_order_release);
    if (err) *err = "Could not activate JACK client.";
    return false;
  }
  active_ = true;
  refreshLatencies();
  return true;
}

void Host::close() {
  live_.store(false, std::memory_order_release);
  if (client_ && api_) {
    if (active_ && !serverShutDown_) api_->deactivate(client_);
    if (!serverShutDown_) {
      for (Port* p : inPort_)
        if (p) api_->port_unregister(client_, p);
      for (Port* p : outPort_)
        if (p) api_->port_unregister(client_, p);
    }
    api_->client_close(client_);
  }
  client_ = nullptr;
  api_ = nullptr;
  active_ = false;
  serverShutDown_ = false;
  inPort_.clear();
  outPort_.clear();
  inAlias_.clear();
  outAlias_.clear();
  plan_ = PortPlan{};
  clientName_.clear();
  fn_ = nullptr;
  user_ = nullptr;
  rate_ = 0;
  bufferFrames_ = 0;
  inLatency_ = 0;
  outLatency_ = 0;
  rawPlayLatency_.store(0, std::memory_order_relaxed);
  freewheel_.store(false, std::memory_order_relaxed);
}

std::string Host::shutdownReason() const {
  if (!reasonReady_.load(std::memory_order_acquire)) return {};
  return std::string(shutdownReason_);
}

int Host::poll() {
  int ev = events_.exchange(0, std::memory_order_acq_rel);
  if (ev & Shutdown) {
    serverShutDown_ = true;
    active_ = false;
    live_.store(false, std::memory_order_release);
    return ev;
  }
  if (!client_ || !api_) return ev;
  if (ev & RateChanged) {
    const int r = pendingRate_.load(std::memory_order_acquire);
    if (r > 0 && r != rate_)
      rate_ = r;
    else
      ev &= ~RateChanged;
  }
  if (ev & BufferChanged) {
    const int b = pendingBuffer_.load(std::memory_order_acquire);
    if (b > 0) bufferFrames_ = b;
  }
  if (ev & (Latency | Graph | BufferChanged)) {
    refreshLatencies();
  }
  return ev;
}

void Host::setEngineOutputLatency(int frames) {
  const int clamped = std::max(0, frames);
  const int prev = engineOutLatency_.exchange(clamped, std::memory_order_acq_rel);
  if (prev != clamped && active_ && !serverShutDown_ && client_ && api_ && api_->recompute_total_latencies) {
    api_->recompute_total_latencies(client_);
  }
}

void Host::refreshLatencies() {
  if (!client_ || !api_ || serverShutDown_ || !api_->port_get_latency_range) {
    inLatency_ = 0;
    outLatency_ = 0;
    return;
  }
  std::vector<LatencyRange> capRanges;
  for (Port* p : inPort_) {
    if (!p) continue;
    LatencyRange r{0, 0};
    api_->port_get_latency_range(p, kCaptureLatency, &r);
    capRanges.push_back(r);
  }
  std::vector<LatencyRange> playRanges;
  // Main pair outputs (Out 1, Out 2) determine the external playback latency.
  const int pdc = engineOutLatency_.load(std::memory_order_relaxed);
  const int rawPlay = rawPlayLatency_.load(std::memory_order_relaxed);
  if (rawPlay > 0) {
    playRanges.push_back({static_cast<Nframes>(rawPlay), static_cast<Nframes>(rawPlay)});
  } else {
    for (size_t i = 0; i < std::min<size_t>(2, outPort_.size()); ++i) {
      if (!outPort_[i]) continue;
      LatencyRange r{0, 0};
      api_->port_get_latency_range(outPort_[i], kPlaybackLatency, &r);
      const Nframes ext = r.max > static_cast<Nframes>(pdc) ? r.max - static_cast<Nframes>(pdc) : 0;
      playRanges.push_back({ext, ext});
    }
  }
  inLatency_ = recordLatency(capRanges, {});
  outLatency_ = recordLatency({}, playRanges);
}

Port* Host::findOwnPort(const std::string& shortName, bool* isInput) const {
  for (size_t i = 0; i < plan_.ins.size() && i < inPort_.size(); ++i) {
    if (plan_.ins[i].name == shortName) {
      if (isInput) *isInput = true;
      return inPort_[i];
    }
  }
  for (size_t i = 0; i < plan_.outs.size() && i < outPort_.size(); ++i) {
    if (plan_.outs[i].name == shortName) {
      if (isInput) *isInput = false;
      return outPort_[i];
    }
  }
  return nullptr;
}

std::vector<PortInfo> Host::queryPorts() const {
  std::vector<PortInfo> out;
  if (!client_ || !api_ || serverShutDown_) return out;
  out.reserve(inPort_.size() + outPort_.size());
  for (size_t i = 0; i < plan_.ins.size() && i < inPort_.size(); ++i) {
    PortInfo pi;
    pi.shortName = plan_.ins[i].name;
    const char* fn = api_->port_name(inPort_[i]);
    pi.fullName = fn ? fn : (clientName_ + ":" + pi.shortName);
    pi.alias = i < inAlias_.size() ? inAlias_[i] : plan_.ins[i].alias;
    pi.input = true;
    pi.channel = plan_.ins[i].channel;
    pi.connected = takeJackStringList(*api_, api_->port_get_all_connections(client_, inPort_[i]));
    out.push_back(std::move(pi));
  }
  for (size_t i = 0; i < plan_.outs.size() && i < outPort_.size(); ++i) {
    PortInfo pi;
    pi.shortName = plan_.outs[i].name;
    const char* fn = api_->port_name(outPort_[i]);
    pi.fullName = fn ? fn : (clientName_ + ":" + pi.shortName);
    pi.alias = i < outAlias_.size() ? outAlias_[i] : plan_.outs[i].alias;
    pi.input = false;
    pi.channel = plan_.outs[i].channel;
    pi.connected = takeJackStringList(*api_, api_->port_get_all_connections(client_, outPort_[i]));
    out.push_back(std::move(pi));
  }
  return out;
}

std::vector<std::string> Host::listExternalPorts(bool wantOutputs) const {
  if (!client_ || !api_ || serverShutDown_) return {};
  const unsigned long flags = wantOutputs ? kPortIsOutput : kPortIsInput;
  std::vector<std::string> all = takeJackStringList(*api_, api_->get_ports(client_, nullptr, kDefaultAudioType, flags));
  const std::string selfPrefix = clientName_ + ":";
  all.erase(std::remove_if(all.begin(), all.end(), [&](const std::string& n) { return n.rfind(selfPrefix, 0) == 0; }),
            all.end());
  return all;
}

bool Host::connect(const std::string& ownShortName, const std::string& peerFullName) {
  if (!client_ || !api_ || !active_ || serverShutDown_ || peerFullName.empty()) return false;
  bool isInput = false;
  Port* p = findOwnPort(ownShortName, &isInput);
  if (!p) return false;
  const char* ownFull = api_->port_name(p);
  if (!ownFull) return false;
  const int r = isInput ? api_->connect(client_, peerFullName.c_str(), ownFull)
                        : api_->connect(client_, ownFull, peerFullName.c_str());
  if (r == 0) {
    refreshLatencies();
    return true;
  }
  return r == 17;  // EEXIST: already connected
}

bool Host::disconnect(const std::string& ownShortName, const std::string& peerFullName) {
  if (!client_ || !api_ || !active_ || serverShutDown_ || peerFullName.empty()) return false;
  bool isInput = false;
  Port* p = findOwnPort(ownShortName, &isInput);
  if (!p) return false;
  const char* ownFull = api_->port_name(p);
  if (!ownFull) return false;
  const int r = isInput ? api_->disconnect(client_, peerFullName.c_str(), ownFull)
                        : api_->disconnect(client_, ownFull, peerFullName.c_str());
  if (r == 0) {
    refreshLatencies();
    return true;
  }
  return false;
}

std::vector<Link> Host::snapshotLinks() const {
  std::vector<Link> out;
  if (!client_ || !api_ || serverShutDown_) return out;
  for (const PortInfo& pi : queryPorts()) {
    for (const std::string& peer : pi.connected) {
      out.push_back({pi.shortName, peer});
    }
  }
  return out;
}

int Host::restoreLinks(const std::vector<Link>& saved) {
  if (!client_ || !api_ || !active_ || serverShutDown_ || saved.empty()) return 0;
  std::vector<std::string> ownPorts;
  for (const auto& s : plan_.ins) ownPorts.push_back(s.name);
  for (const auto& s : plan_.outs) ownPorts.push_back(s.name);

  std::vector<std::string> peers = listExternalPorts(true);
  for (const auto& p : listExternalPorts(false)) peers.push_back(p);

  const std::vector<Link> current = snapshotLinks();
  const std::vector<Link> todo = linksToRestore(saved, ownPorts, peers, current);
  int made = 0;
  for (const Link& l : todo) {
    if (connect(l.own, l.peer)) ++made;
  }
  return made;
}

void Host::autoConnect(bool connectOutputs, bool connectInputs) {
  if (!client_ || !api_ || !active_ || serverShutDown_) return;
  if (connectOutputs && outPort_.size() >= 2) {
    const std::vector<std::string> phys = takeJackStringList(
        *api_, api_->get_ports(client_, nullptr, kDefaultAudioType, kPortIsPhysical | kPortIsInput));
    const std::vector<std::string> picked = pickPhysical(phys, 2);
    for (size_t i = 0; i < picked.size() && i < 2; ++i) {
      connect(plan_.outs[i].name, picked[i]);
    }
  }
  if (connectInputs && !inPort_.empty()) {
    const std::vector<std::string> phys = takeJackStringList(
        *api_, api_->get_ports(client_, nullptr, kDefaultAudioType, kPortIsPhysical | kPortIsOutput));
    const std::vector<std::string> picked = pickPhysical(phys, static_cast<int>(inPort_.size()));
    for (size_t i = 0; i < picked.size() && i < plan_.ins.size(); ++i) {
      connect(plan_.ins[i].name, picked[i]);
    }
  }
}

void Host::applyAliasAndMeta(Port* port, std::string& currentAlias, const std::string& nextAlias) {
  if (!port || !client_ || !api_ || serverShutDown_) return;
  const std::string clean = sanitizeName(nextAlias, kMaxPortNameLen, "");
  if (clean == currentAlias) return;
  if (!currentAlias.empty() && api_->port_unset_alias) {
    api_->port_unset_alias(port, currentAlias.c_str());
  }
  if (!clean.empty() && api_->port_set_alias) {
    api_->port_set_alias(port, clean.c_str());
  }
  if (api_->port_uuid && api_->set_property) {
    const Uuid u = api_->port_uuid(port);
    if (u != 0) {
      if (!clean.empty())
        api_->set_property(client_, u, kMetadataPrettyName, clean.c_str(), "text/plain");
      else if (api_->remove_property)
        api_->remove_property(client_, u, kMetadataPrettyName);
    }
  }
  currentAlias = clean;
}

bool Host::setPortAlias(const std::string& ownShortName, const std::string& newAlias) {
  if (!client_ || !api_ || serverShutDown_) return false;
  for (size_t i = 0; i < plan_.ins.size() && i < inPort_.size(); ++i) {
    if (plan_.ins[i].name == ownShortName) {
      applyAliasAndMeta(inPort_[i], inAlias_[i], newAlias);
      return true;
    }
  }
  for (size_t i = 0; i < plan_.outs.size() && i < outPort_.size(); ++i) {
    if (plan_.outs[i].name == ownShortName) {
      applyAliasAndMeta(outPort_[i], outAlias_[i], newAlias);
      return true;
    }
  }
  return false;
}

void Host::updateInputAliases(const std::vector<TrackInputRef>& tracks) {
  if (!client_ || !api_ || serverShutDown_) return;
  const std::vector<std::string> aliases = planInputAliases(plan_, tracks);
  for (size_t i = 0; i < aliases.size() && i < inPort_.size(); ++i) {
    applyAliasAndMeta(inPort_[i], inAlias_[i], aliases[i]);
  }
}

// ---- realtime & server notification callbacks (no allocations, no locks, no I/O) ----

int Host::onProcess(Nframes nframes, void* arg) {
  auto* h = static_cast<Host*>(arg);
  if (!h || !h->live_.load(std::memory_order_acquire) || !h->api_ || !h->fn_) return 0;

  const int nIn = std::min<int>(static_cast<int>(h->inPort_.size()), kMaxInputs);
  const int nOut = std::min<int>(static_cast<int>(h->outPort_.size()), 2 + 2 * kMaxExtraPairs);

  const float* inPtrs[kMaxInputs];
  float* outPtrs[2 + 2 * kMaxExtraPairs];
  for (int i = 0; i < nIn; ++i) {
    inPtrs[i] = static_cast<const float*>(h->api_->port_get_buffer(h->inPort_[static_cast<size_t>(i)], nframes));
  }
  for (int i = 0; i < nOut; ++i) {
    outPtrs[i] = static_cast<float*>(h->api_->port_get_buffer(h->outPort_[static_cast<size_t>(i)], nframes));
  }

  h->fn_(h->user_, inPtrs, nIn, outPtrs, nOut, static_cast<int>(nframes));
  h->cycles_.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

int Host::onBufferSize(Nframes nframes, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->pendingBuffer_.store(static_cast<int>(nframes), std::memory_order_release);
    h->events_.fetch_or(BufferChanged, std::memory_order_acq_rel);
  }
  return 0;
}

int Host::onSampleRate(Nframes nframes, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->pendingRate_.store(static_cast<int>(nframes), std::memory_order_release);
    h->events_.fetch_or(RateChanged, std::memory_order_acq_rel);
  }
  return 0;
}

int Host::onXrun(void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    if (!h->freewheel_.load(std::memory_order_relaxed)) {
      h->xruns_.fetch_add(1, std::memory_order_relaxed);
      h->events_.fetch_or(Xrun, std::memory_order_acq_rel);
    }
  }
  return 0;
}

void Host::onFreewheel(int starting, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->freewheel_.store(starting != 0, std::memory_order_relaxed);
    h->events_.fetch_or(Freewheel, std::memory_order_acq_rel);
  }
}

void Host::onShutdown(void* arg) { onInfoShutdown(kStatusFailure, "server shut down", arg); }

void Host::onInfoShutdown(int, const char* reason, void* arg) {
  auto* h = static_cast<Host*>(arg);
  if (!h) return;
  h->live_.store(false, std::memory_order_release);
  if (!h->reasonReady_.load(std::memory_order_relaxed)) {
    const char* src = (reason && *reason) ? reason : "server shut down";
    const size_t n = std::min(std::strlen(src), sizeof(h->shutdownReason_) - 1);
    std::memcpy(h->shutdownReason_, src, n);
    h->shutdownReason_[n] = '\0';
    h->reasonReady_.store(true, std::memory_order_release);
  }
  h->events_.fetch_or(Shutdown, std::memory_order_acq_rel);
}

void Host::onLatency(int mode, void* arg) {
  auto* h = static_cast<Host*>(arg);
  if (!h || !h->api_ || !h->api_->port_get_latency_range || !h->api_->port_set_latency_range) return;
  if (mode == kCaptureLatency) {
    // Capture latency of our inputs comes from upstream; pass through as received and reflect on outputs.
    LatencyRange capMax{0, 0};
    bool any = false;
    for (Port* p : h->inPort_) {
      if (!p) continue;
      LatencyRange r{0, 0};
      h->api_->port_get_latency_range(p, kCaptureLatency, &r);
      if (!any) {
        capMax = r;
        any = true;
      } else {
        capMax.min = std::min(capMax.min, r.min);
        capMax.max = std::max(capMax.max, r.max);
      }
    }
    const Nframes pdc = static_cast<Nframes>(std::max(0, h->engineOutLatency_.load(std::memory_order_relaxed)));
    LatencyRange outCap{capMax.min + pdc, capMax.max + pdc};
    for (Port* p : h->outPort_) {
      if (p) h->api_->port_set_latency_range(p, kCaptureLatency, &outCap);
    }
  } else if (mode == kPlaybackLatency) {
    const Nframes pdc = static_cast<Nframes>(std::max(0, h->engineOutLatency_.load(std::memory_order_relaxed)));
    Nframes mainRawMax = 0;
    for (size_t i = 0; i < h->outPort_.size(); ++i) {
      Port* p = h->outPort_[i];
      if (!p) continue;
      LatencyRange r{0, 0};
      h->api_->port_get_latency_range(p, kPlaybackLatency, &r);
      if (i < 2) mainRawMax = std::max(mainRawMax, r.max);
      LatencyRange reported{r.min + pdc, r.max + pdc};
      h->api_->port_set_latency_range(p, kPlaybackLatency, &reported);
    }
    h->rawPlayLatency_.store(static_cast<int>(mainRawMax), std::memory_order_relaxed);
  }
  h->events_.fetch_or(Latency, std::memory_order_acq_rel);
}

void Host::onClientRegistration(const char*, int reg, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->events_.fetch_or(reg ? (Graph | ClientAppeared) : Graph, std::memory_order_acq_rel);
  }
}

void Host::onPortRegistration(PortId, int reg, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->events_.fetch_or(reg ? (Graph | ClientAppeared) : Graph, std::memory_order_acq_rel);
  }
}

void Host::onPortConnect(PortId, PortId, int, void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->events_.fetch_or(Graph | Latency, std::memory_order_acq_rel);
  }
}

int Host::onGraphOrder(void* arg) {
  if (auto* h = static_cast<Host*>(arg)) {
    h->events_.fetch_or(Graph, std::memory_order_acq_rel);
  }
  return 0;
}

// ---- RetryWorker ----

void RetryWorker::start(const Api* api, int intervalMs) {
  stop();
  if (!api || !apiComplete(*api)) return;
  serverBack_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  const int ms = std::max(100, intervalMs);
  thread_ = std::thread([this, api, ms]() {
    gl::threads::setName("gl-jack-retry");
    run(api, ms);
  });
}

void RetryWorker::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void RetryWorker::run(const Api* api, int intervalMs) {
  std::unique_lock<std::mutex> lock(mu_);
  while (running_.load(std::memory_order_acquire)) {
    cv_.wait_for(lock, std::chrono::milliseconds(intervalMs),
                 [this]() { return !running_.load(std::memory_order_acquire); });
    if (!running_.load(std::memory_order_acquire)) break;
    lock.unlock();
    int st = 0;
    Client* probe = api->client_open("Gridline-probe", kNoStartServer, &st);
    if (probe) {
      api->client_close(probe);
      serverBack_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }
    lock.lock();
  }
}

}  // namespace gl::jack

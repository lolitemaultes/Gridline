// Gridline - J-111 collaboration: in-process endpoints for the tests. See Link.h.
#include <cstdlib>
#include <random>
#include <set>
#include "Link.h"

namespace gl::collab {

struct MemFabric::Shared {
  std::mutex mu;
  std::map<int, Endpoint*> eps;
  int nextId = 1;
  std::mt19937 rng{1};
  bool natBoth = false;
  double loss = 0;
  std::set<std::pair<std::string, std::string>> blocked;
};

namespace {

class MemEndpoint : public Endpoint {
 public:
  MemEndpoint(std::shared_ptr<MemFabric::Shared> sh, int id, std::string label, uint16_t port)
      : sh_(std::move(sh)), id_(id), label_(std::move(label)), port_(port) {}
  ~MemEndpoint() override {
    std::lock_guard<std::mutex> lk(sh_->mu);
    sh_->eps.erase(id_);
    for (auto& [k, e] : sh_->eps) {
      auto* m = static_cast<MemEndpoint*>(e);
      if (m->bound_ == this) m->bound_ = nullptr;
    }
  }
  void gather() override { gathered_ = true; }
  bool gathered() const override { return gathered_; }
  // an "address": the computer and its UDP port (stable across restarts when the port is reused)
  std::string localDescription() const override { return "mem:" + label_ + ":" + std::to_string(port_); }
  void setRemote(const std::string& d) override {
    if (d.rfind("mem:", 0) != 0) return;
    const size_t c = d.rfind(':');
    if (c == std::string::npos || c < 4) return;
    std::lock_guard<std::mutex> lk(sh_->mu);
    remoteLabel_ = d.substr(4, c - 4);
    remotePort_ = static_cast<uint16_t>(std::atoi(d.c_str() + c + 1));
  }
  bool knows(const MemEndpoint* m) const { return remotePort_ && remotePort_ == m->port_ && remoteLabel_ == m->label_; }
  // connected when the other side knows us (and, behind "NAT", we know it too); a pair stays bound
  MemEndpoint* peerLocked() {
    if (bound_) return bound_;
    for (const auto& [id, e] : sh_->eps) {
      auto* m = static_cast<MemEndpoint*>(e);
      if (m == this || m->bound_) continue;
      if (sh_->blocked.count({label_, m->label_}) || sh_->blocked.count({m->label_, label_})) continue;
      const bool weKnow = knows(m), theyKnow = m->knows(this);
      if (sh_->natBoth ? (weKnow && theyKnow) : (weKnow || theyKnow)) {
        bound_ = m;
        m->bound_ = this;
        return m;
      }
    }
    return nullptr;
  }
  void send(const std::string& d) override {
    MemEndpoint* p = nullptr;
    {
      std::lock_guard<std::mutex> lk(sh_->mu);
      p = peerLocked();
      if (p && sh_->loss > 0 && std::uniform_real_distribution<double>(0, 1)(sh_->rng) < sh_->loss) return;
    }
    if (p && p->onReceive) p->onReceive(d);
  }
  State state() const override {
    std::lock_guard<std::mutex> lk(sh_->mu);
    if (const_cast<MemEndpoint*>(this)->peerLocked()) return State::Connected;
    if (remotePort_) return State::Connecting;
    return gathered_ ? State::Gathering : State::New;
  }
  std::string path() const override { return state() == State::Connected ? "host" : ""; }
  uint16_t localPort() const override { return port_; }

  std::shared_ptr<MemFabric::Shared> sh_;
  int id_;
  std::string label_;
  uint16_t port_;
  std::string remoteLabel_;
  uint16_t remotePort_ = 0;
  MemEndpoint* bound_ = nullptr;
  bool gathered_ = false;
};

}  // namespace

std::shared_ptr<MemFabric::Shared> MemFabric::makeNet() { return std::make_shared<Shared>(); }

void MemFabric::block(const std::shared_ptr<Shared>& net, const std::string& a, const std::string& b) {
  std::lock_guard<std::mutex> lk(net->mu);
  net->blocked.insert({a, b});
}

std::shared_ptr<MemFabric::Shared> MemFabric::shared() {
  if (!shared_) shared_ = std::make_shared<Shared>();
  if (natBoth) shared_->natBoth = true;
  if (loss > 0) shared_->loss = loss;
  return shared_;
}

std::unique_ptr<Endpoint> MemFabric::create(const EndpointConfig& cfg) {
  auto sh = shared();
  std::lock_guard<std::mutex> lk(sh->mu);
  if (sh->nextId == 1) sh->rng.seed(seed);
  const int id = sh->nextId++;
  // the requested port when free (reconnects), else a new one
  uint16_t port = cfg.localPort;
  auto taken = [&](uint16_t p) {
    for (const auto& [k, ep] : sh->eps) {
      auto* m = static_cast<MemEndpoint*>(ep);
      if (m->port_ == p && m->label_ == label_) return true;
    }
    return false;
  };
  if (!port || taken(port))
    do port = static_cast<uint16_t>(20000 + (sh->rng() % 40000)); while (taken(port));
  auto e = std::make_unique<MemEndpoint>(sh, id, label_, port);
  sh->eps[id] = e.get();
  return e;
}

}  // namespace gl::collab

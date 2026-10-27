// Gridline - J-146 collaboration: presence on the welcome screen. See Presence.h.
#include "Presence.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include "Debug.h"
#include "Hub.h"
#include "Log.h"
#include "Session.h"
#include "Transport.h"

namespace fs = std::filesystem;

namespace gl::collab {

namespace {

std::string keyHex(const crypto::Key& k) { return crypto::toHex(k.data(), 32); }
bool hexKey(const std::string& h, crypto::Key& k) {
  std::string b;
  return crypto::fromHex(h, b) && crypto::keyFrom(b, k);
}

const char* memberStateName(CardMember::State s) {
  switch (s) {
    case CardMember::State::Active: return "active";
    case CardMember::State::Removed: return "removed";
    case CardMember::State::Left: return "left";
  }
  return "active";
}

std::string projectFile(const std::string& dir, const std::string& projectId, const char* ext) {
  return (fs::path(dir) / "projects" / (projectId + ext)).string();
}

// A project id is a hex string: never a path (it names files of this computer).
bool safeId(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (char c : id)
    if (!std::isxdigit(static_cast<unsigned char>(c)) && c != '-') return false;
  return true;
}

}  // namespace

// ------------------------------------------------------------------ card ----
const CardMember* ProjectCard::member(SiteId s) const {
  for (const auto& m : members)
    if (m.site == s) return &m;
  return nullptr;
}

ProjectCard cardOf(const Session& s, uint64_t nowMs) {
  ProjectCard c;
  c.id = s.projectId();
  c.name = s.projectName();
  c.folder = s.config().folder;
  c.me = s.site();
  c.admin = s.admin();
  for (const Member& m : s.members()) {
    CardMember cm;
    cm.site = m.site;
    cm.signPublic = m.signPublic;
    cm.dhPublic = m.dhPublic;
    cm.name = m.name;
    const MemberState st = s.memberState(m.site);
    cm.state = st == MemberState::Removed ? CardMember::State::Removed : st == MemberState::Left ? CardMember::State::Left : CardMember::State::Active;
    if (m.site == s.site()) {
      cm.name = cleanName(s.config().profile.name).empty() ? m.name : cleanName(s.config().profile.name);
      cm.color = s.config().profile.color;
      cm.avatarHash = s.config().profile.avatarHash();
    } else if (auto it = s.peers().find(m.site); it != s.peers().end()) {
      const PeerView& pv = it->second;
      if (!pv.profile.name.empty()) cm.name = pv.profile.name;
      cm.color = pv.profile.color;
      cm.avatarHash = pv.profile.avatarHash();
      cm.lastSyncMs = pv.lastSyncMs;
    }
    c.members.push_back(cm);
  }
  c.pending = s.unackedBatches();
  c.review = s.isAdmin() ? s.conflicts().openCount() : 0;
  c.minAppVersion = s.minAppVersion();
  c.lastContactMs = s.lastContactMs();
  c.savedAt = nowMs;
  return c;
}

Json cardToJson(const ProjectCard& c) {
  Json j = Json::makeObject();
  j.object["v"] = Json::makeNumber(1);
  j.object["id"] = Json::makeString(c.id);
  j.object["name"] = Json::makeString(c.name);
  j.object["folder"] = Json::makeString(c.folder);
  j.object["me"] = Json::makeString(siteHex(c.me));
  j.object["admin"] = Json::makeString(siteHex(c.admin));
  Json ms = Json::makeArray();
  for (const auto& m : c.members) {
    Json e = Json::makeObject();
    e.object["site"] = Json::makeString(siteHex(m.site));
    e.object["sign"] = Json::makeString(keyHex(m.signPublic));
    e.object["dh"] = Json::makeString(keyHex(m.dhPublic));
    e.object["name"] = Json::makeString(m.name);
    e.object["color"] = Json::makeNumber(m.color & 0xFFFFFF);
    if (!m.avatarHash.empty()) e.object["avatar"] = Json::makeString(m.avatarHash);
    e.object["state"] = Json::makeString(memberStateName(m.state));
    e.object["sync"] = Json::makeNumber(static_cast<double>(m.lastSyncMs));
    ms.array.push_back(std::move(e));
  }
  j.object["members"] = std::move(ms);
  j.object["pending"] = Json::makeNumber(static_cast<double>(c.pending));
  j.object["review"] = Json::makeNumber(static_cast<double>(c.review));
  j.object["minApp"] = Json::makeString(c.minAppVersion);
  j.object["contact"] = Json::makeNumber(static_cast<double>(c.lastContactMs));
  j.object["saved"] = Json::makeNumber(static_cast<double>(c.savedAt));
  return j;
}

bool cardFromJson(const Json& j, ProjectCard& out) {
  if (!j.is(Json::Type::Object) || j.num("v", 0) < 1) return false;
  ProjectCard c;
  c.id = j.text("id");
  c.name = j.text("name");
  c.folder = j.text("folder");
  c.me = siteFromHex(j.text("me"));
  c.admin = siteFromHex(j.text("admin"));
  if (const Json* ms = j.find("members"); ms && ms->is(Json::Type::Array))
    for (const auto& e : ms->array) {
      CardMember m;
      m.site = siteFromHex(e.text("site"));
      if (!m.site || !hexKey(e.text("sign"), m.signPublic) || !hexKey(e.text("dh"), m.dhPublic)) continue;
      if (crypto::siteOf(m.signPublic) != m.site) continue;  // a site is its signing key's
      m.name = cleanName(e.text("name"));
      m.color = static_cast<uint32_t>(std::clamp(e.num("color", 0), 0.0, 16777215.0));
      m.avatarHash = e.text("avatar");
      const std::string st = e.text("state");
      m.state = st == "removed" ? CardMember::State::Removed : st == "left" ? CardMember::State::Left : CardMember::State::Active;
      m.lastSyncMs = static_cast<uint64_t>(std::max(0.0, e.num("sync", 0)));
      c.members.push_back(m);
    }
  c.pending = static_cast<size_t>(std::max(0.0, j.num("pending", 0)));
  c.review = static_cast<size_t>(std::max(0.0, j.num("review", 0)));
  c.minAppVersion = j.text("minApp");
  c.lastContactMs = static_cast<uint64_t>(std::max(0.0, j.num("contact", 0)));
  c.savedAt = static_cast<uint64_t>(std::max(0.0, j.num("saved", 0)));
  if (!c.valid()) return false;
  out = std::move(c);
  return true;
}

std::string cardPath(const std::string& collabDir, const std::string& projectId) { return projectFile(collabDir, projectId, ".card.json"); }

bool writeCard(const std::string& collabDir, const ProjectCard& c) {
  if (collabDir.empty() || !safeId(c.id)) return false;
  std::error_code ec;
  fs::create_directories(fs::path(collabDir) / "projects", ec);
  return writeFileDurable(cardPath(collabDir, c.id), jsonStringify(cardToJson(c), 1));
}

bool readCard(const std::string& collabDir, const std::string& projectId, ProjectCard& out) {
  if (collabDir.empty() || !safeId(projectId)) return false;
  std::string t;
  Json j;
  if (!readFileText(cardPath(collabDir, projectId), t, 1 << 20) || !jsonParse(t, j)) return false;
  ProjectCard c;
  if (!cardFromJson(j, c) || c.id != projectId) return false;
  out = std::move(c);
  return true;
}

std::map<SiteId, int64_t> readSeen(const std::string& collabDir, const std::string& projectId) {
  std::map<SiteId, int64_t> out;
  if (collabDir.empty() || !safeId(projectId)) return out;
  std::string t;
  Json j;
  if (!readFileText(projectFile(collabDir, projectId, ".seen.json"), t, 1 << 18) || !jsonParse(t, j) || !j.is(Json::Type::Object)) return out;
  for (const auto& [k, v] : j.object)
    if (const SiteId s = siteFromHex(k); s && v.is(Json::Type::Number) && v.number > 0) out[s] = static_cast<int64_t>(v.number);
  return out;
}

bool writeSeen(const std::string& collabDir, const std::string& projectId, const std::map<SiteId, int64_t>& seen) {
  if (collabDir.empty() || !safeId(projectId)) return false;
  Json j = Json::makeObject();
  for (const auto& [s, t] : seen) j.object[siteHex(s)] = Json::makeNumber(static_cast<double>(t));
  std::error_code ec;
  fs::create_directories(fs::path(collabDir) / "projects", ec);
  return writeFileDurable(projectFile(collabDir, projectId, ".seen.json"), jsonStringify(j, 1));
}

// --------------------------------------------------------------- results ----
const char* presenceStateName(PresenceState s) {
  switch (s) {
    case PresenceState::Unknown: return "unknown";
    case PresenceState::Checking: return "checking";
    case PresenceState::Working: return "working";
    case PresenceState::OtherVersion: return "other-version";
    case PresenceState::Online: return "online";
    case PresenceState::NoAnswer: return "no-answer";
    case PresenceState::Hidden: return "hidden";
    case PresenceState::RemovedMe: return "removed-me";
    case PresenceState::Ended: return "ended";
  }
  return "unknown";
}

double nextProbeDelay(PresenceState s, int failures) {
  switch (s) {
    case PresenceState::Working:
    case PresenceState::Online:
    case PresenceState::Hidden: return 20;
    case PresenceState::OtherVersion: return 120;
    case PresenceState::RemovedMe:
    case PresenceState::Ended: return 600;
    case PresenceState::NoAnswer: {
      const int f = std::clamp(failures, 1, 6);
      return std::min(300.0, 15.0 * std::pow(2.0, f - 1));  // 15, 30, 60, 120, 240, then 300
    }
    case PresenceState::Unknown:
    case PresenceState::Checking: return 60;
  }
  return 60;
}

// ---------------------------------------------------------------- prober ----
struct PresenceProber::Probe {
  struct Inbox {
    std::mutex mu;
    std::deque<std::string> q;
  };
  ProbeTarget t;
  std::unique_ptr<Endpoint> ep;
  std::unique_ptr<SecureLink> sl;
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  bool initiator = false, started = false, done = false, heard = false;
  bool silent = false;  // responder: "no answer" already shown, still listening
  Json hello;  // responder: the waiting link's hello (their Hub is the initiator)
  double created = 0, lingerUntil = -1;
};

PresenceProber::PresenceProber(std::unique_ptr<Fabric> fabric, const crypto::Identity& id, Profile profile, VersionInfo version)
    : fabric_(std::move(fabric)), id_(id), profile_(std::move(profile)), app_(version.app), build_(version.build), protocol_(version.protocol) {
  wallSec = [] { return static_cast<int64_t>(std::time(nullptr)); };
}

PresenceProber::~PresenceProber() { probes_.clear(); }

std::string PresenceProber::key(const std::string& pid, SiteId s) const { return pid + "/" + siteHex(s); }

void PresenceProber::setTargets(std::vector<ProbeTarget> targets) {
  targets_ = std::move(targets);
  std::set<std::string> keep;
  for (const auto& t : targets_) keep.insert(key(t.projectId, t.site));
  probes_.erase(std::remove_if(probes_.begin(), probes_.end(), [&](const std::unique_ptr<Probe>& p) { return !keep.count(key(p->t.projectId, p->t.site)); }),
                probes_.end());
}

void PresenceProber::setEnabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  if (!on) {
    probes_.clear();
    results_.clear();
    nextAt_.clear();
    changed_ = true;
  }
}

void PresenceProber::want(double now) {
  if (now_ < now) now_ = now;
  const bool was = running_ || (wantUntil_ >= 0 && now <= wantUntil_);
  wantUntil_ = now + kPresenceIdleStopSec;
  if (!was && now - lastRefresh_ > 60) refreshNow();  // the view opened again: unanswered members are asked again now
}

void PresenceProber::refreshNow() {
  lastRefresh_ = now_;
  for (auto& [k, at] : nextAt_) at = std::min(at, now_);
}

void PresenceProber::forget(const std::string& projectId) {
  probes_.erase(std::remove_if(probes_.begin(), probes_.end(), [&](const std::unique_ptr<Probe>& p) { return p->t.projectId == projectId; }),
                probes_.end());
  targets_.erase(std::remove_if(targets_.begin(), targets_.end(), [&](const ProbeTarget& t) { return t.projectId == projectId; }), targets_.end());
}

const PresenceResult* PresenceProber::result(const std::string& projectId, SiteId site) const {
  auto it = results_.find(projectId);
  if (it == results_.end()) return nullptr;
  auto r = it->second.find(site);
  return r == it->second.end() ? nullptr : &r->second;
}

const std::map<SiteId, PresenceResult>* PresenceProber::results(const std::string& projectId) const {
  auto it = results_.find(projectId);
  return it == results_.end() ? nullptr : &it->second;
}

bool PresenceProber::takeChanged() {
  const bool c = changed_;
  changed_ = false;
  return c;
}

Json PresenceProber::helloJson(const std::string& projectId) const {
  Json h = Json::makeObject();
  h.object["site"] = Json::makeString(siteHex(id_.site()));
  h.object["sign"] = Json::makeString(keyHex(id_.signPublic));
  h.object["app"] = Json::makeString(app_);
  h.object["protocol"] = Json::makeNumber(protocol_);
  h.object["build"] = Json::makeString(build_);
  h.object["profile"] = profileWire(profile_);
  h.object["project"] = Json::makeString(projectId);
  h.object["probe"] = Json::makeNumber(1);
  return h;
}

void PresenceProber::startProbe(const ProbeTarget& t, double now) {
  ++started_;
  auto p = std::make_unique<Probe>();
  p->t = t;
  p->created = now;
  // the same roles as member links: the lower site starts the handshake (ICE controlling)
  p->initiator = id_.site() < t.site;
  const crypto::Key psk = memberLinkPsk(id_, t.dhPublic, t.projectId);
  EndpointConfig ec = memberLinkEndpoint(psk, p->initiator, t.localPort);
  ec.tag = "presence probe " + siteHex(t.site).substr(0, 8);
  p->ep = fabric_ ? fabric_->create(ec) : nullptr;
  if (!p->ep && t.localPort) {  // that port is busy here: any port (their checks still find us)
    ec.localPort = 0;
    p->ep = fabric_ ? fabric_->create(ec) : nullptr;
  }
  PresenceResult& r = results_[t.projectId][t.site];
  if (!p->ep) {
    r.state = PresenceState::NoAnswer;
    r.checkedAt = wallSec();
    ++r.failures;
    nextAt_[key(t.projectId, t.site)] = now + nextProbeDelay(r.state, r.failures);
    changed_ = true;
    return;
  }
  if (r.state == PresenceState::Unknown) {
    r.state = PresenceState::Checking;
    changed_ = true;
  }
  SecureLink::Hs hs;
  hs.initiator = p->initiator;
  hs.psk = psk;
  hs.hsid = memberLinkHsid(psk);
  hs.me = id_;
  hs.remoteDh = t.dhPublic;
  hs.remoteDhKnown = true;
  hs.hello = helloJson(t.projectId);
  Probe* raw = p.get();
  // Responder: their open project's waiting link starts the handshake with its member
  // hello (it says which project is open, since when); we refuse it ("probe").
  hs.accept = [this, raw](const Json& theirs, const crypto::Key&, bool& ok) {
    ok = false;
    raw->hello = theirs;
    raw->heard = true;
    Json r = helloJson(raw->t.projectId);
    r.object["reason"] = Json::makeString("probe");
    return r;
  };
  p->sl = std::make_unique<SecureLink>(std::move(hs));
  p->sl->tag = ec.tag;
  std::weak_ptr<Probe::Inbox> wi = p->inbox;
  p->ep->onReceive = [wi](const std::string& d) {
    if (auto in = wi.lock()) {
      std::lock_guard<std::mutex> lk(in->mu);
      if (in->q.size() < 256) in->q.push_back(d);
    }
  };
  Endpoint* ep = p->ep.get();
  p->sl->output = [ep](const std::string& d) { ep->send(d); };
  p->ep->gather();
  if (!t.lastDesc.empty()) p->ep->setRemote(t.lastDesc);
  debugf("%s started (%s, %s)", ec.tag.c_str(), p->initiator ? "initiator" : "responder", t.lastDesc.empty() ? "waiting for them" : "their last addresses");
  probes_.push_back(std::move(p));
}

void PresenceProber::applyAnswer(Probe& p, const Json& a, double now) {
  PresenceResult& r = results_[p.t.projectId][p.t.site];
  const int64_t wall = wallSec();
  if (a.num("presence", 1) == 0) {
    r = PresenceResult();
    r.state = PresenceState::Hidden;
    r.checkedAt = wall;
    return;
  }
  Profile prof;
  if (const Json* pj = a.find("profile")) profileFromWire(*pj, prof);
  VersionInfo tv;
  tv.app = a.text("app");
  tv.protocol = static_cast<int>(a.num("protocol", 0));
  tv.build = a.text("build");
  VersionInfo mine;
  mine.app = app_;
  mine.protocol = protocol_;
  mine.build = build_;
  PresenceResult n;
  n.checkedAt = wall;
  n.profile = prof;
  n.app = tv.app;
  if (!versionsMatch(tv, mine)) {
    n.state = PresenceState::OtherVersion;
  } else {
    n.state = PresenceState::Working;
    n.since = static_cast<int64_t>(a.num("since", 0));
    n.idle = static_cast<int64_t>(a.num("idle", -1));
    if (const Json* w = a.find("with"); w && w->is(Json::Type::Array))
      for (const auto& s : w->array)
        if (const SiteId x = siteFromHex(s.str); x && x != id_.site() && x != p.t.site) n.with.push_back(x);
  }
  r = n;
  // the members linked with them are working on it too (as they told us, authenticated)
  if (n.state == PresenceState::Working)
    for (SiteId x : n.with) {
      bool member = false;
      for (const auto& t : targets_) member = member || (t.projectId == p.t.projectId && t.site == x);
      if (!member) continue;
      PresenceResult& o = results_[p.t.projectId][x];
      if (o.state == PresenceState::Working && o.via == 0 && wall - o.checkedAt < kPresenceFreshSec) continue;  // their own answer is fresh
      const int f = o.failures;
      const Profile keep = o.profile;
      o = PresenceResult();
      o.state = PresenceState::Working;
      o.checkedAt = wall;
      o.via = p.t.site;
      o.viaName = prof.name.empty() ? p.t.name : prof.name;
      o.failures = f;
      o.profile = keep;
    }
  (void)now;
}

void PresenceProber::finish(Probe& p, PresenceState st, const Json* answer, double now) {
  if (p.done) return;
  p.done = true;
  p.lingerUntil = now + 1.5;  // a responder repeats its refusal for a moment (lost datagrams)
  PresenceResult& r = results_[p.t.projectId][p.t.site];
  const int failures = r.failures;
  if (answer && (st == PresenceState::Working || st == PresenceState::Online)) {
    if (st == PresenceState::Working) {
      applyAnswer(p, *answer, now);
    } else {
      PresenceResult n;
      n.state = answer->num("presence", 1) == 0 ? PresenceState::Hidden : PresenceState::Online;
      n.checkedAt = wallSec();
      n.app = answer->text("app");
      if (const Json* pj = answer->find("profile")) profileFromWire(*pj, n.profile);
      r = n;
    }
    r.failures = 0;
  } else {
    const Profile keep = r.profile;
    const int64_t since = r.since;
    r = PresenceResult();
    r.state = st;
    r.checkedAt = wallSec();
    r.profile = keep;
    r.failures = st == PresenceState::NoAnswer ? failures + 1 : 0;
    if (answer) {
      r.app = answer->text("app");
      r.byName = cleanName(answer->text("byName"));
    }
    (void)since;
  }
  nextAt_[key(p.t.projectId, p.t.site)] = now + nextProbeDelay(r.state, r.failures);
  changed_ = true;
  debugf("presence probe %s: %s", siteHex(p.t.site).substr(0, 8).c_str(), presenceStateName(r.state));
}

void PresenceProber::poll(double now) {
  if (now > now_) now_ = now;
  const bool run = enabled_ && fabric_ && wantUntil_ >= 0 && now <= wantUntil_;
  if (!run) {
    if (!probes_.empty()) probes_.clear();  // the view is gone: every socket closes
    running_ = false;
    return;
  }
  running_ = true;
  const uint64_t ms = static_cast<uint64_t>(now * 1000.0);
  for (auto& pp : probes_) {
    Probe& p = *pp;
    std::deque<std::string> in;
    {
      std::lock_guard<std::mutex> lk(p.inbox->mu);
      in.swap(p.inbox->q);
    }
    for (const auto& d : in) p.sl->input(d, ms);
    if (!p.started && (!p.initiator || p.ep->state() == Endpoint::State::Connected)) {
      p.started = true;
      p.sl->start(ms);
    }
    p.sl->tick(ms);
    if (p.done) continue;
    if (!p.initiator && p.heard) {
      const Json& h = p.hello;
      const Json* notice = h.find("notice");  // their link carries the news of our removal / the end
      const std::string nr = notice ? notice->text("reason") : std::string();
      if (nr == "removed") finish(p, PresenceState::RemovedMe, notice, now);
      else if (nr == "ended") finish(p, PresenceState::Ended, notice, now);
      else if (h.num("probe", 0) != 0) finish(p, PresenceState::Online, &h, now);  // a welcome screen, like us
      else if (h.text("project") == p.t.projectId) finish(p, PresenceState::Working, &h, now);  // their project's member link
      else finish(p, PresenceState::Unknown, nullptr, now);
      continue;
    }
    const SecureLink::State st = p.sl->state();
    if (p.initiator && st == SecureLink::State::Rejected) {
      const Json& r = p.sl->refusal();
      const std::string reason = r.text("reason");
      if (reason == "presence" || reason == "version") finish(p, PresenceState::Working, &r, now);  // (applyAnswer tells the versions apart)
      else if (reason == "removed") finish(p, PresenceState::RemovedMe, &r, now);
      else if (reason == "ended") finish(p, PresenceState::Ended, &r, now);
      else if (reason == "probe") finish(p, PresenceState::Online, &r, now);
      else finish(p, PresenceState::Unknown, &r, now);
    } else if (st == SecureLink::State::Open) {
      // a peer that opened the link (never between matching builds): closed before anything is said
      const Json h = p.sl->remoteHello();
      p.sl->close("probe");
      finish(p, PresenceState::Working, &h, now);
    } else if (st == SecureLink::State::Closed || now - p.created > (p.initiator ? kProbeTimeoutSec : kProbeListenSec)) {
      finish(p, PresenceState::NoAnswer, nullptr, now);  // never "offline": silence looks the same either way
    } else if (!p.initiator && !p.silent && now - p.created > kProbeTimeoutSec) {
      // a responder says "no answer" after the usual time but keeps listening: their Hub
      // starts a removed member's link (the news of the removal) only once a minute
      p.silent = true;
      PresenceResult& r = results_[p.t.projectId][p.t.site];
      const Profile keep = r.profile;
      const int failures = r.failures;
      r = PresenceResult();
      r.state = PresenceState::NoAnswer;
      r.checkedAt = wallSec();
      r.profile = keep;
      r.failures = failures;
      changed_ = true;
    }
  }
  probes_.erase(std::remove_if(probes_.begin(), probes_.end(), [&](const std::unique_ptr<Probe>& p) { return p->done && now >= p->lingerUntil; }),
                probes_.end());
  // start the probes that are due (a few at a time)
  for (const auto& t : targets_) {
    if (static_cast<int>(probes_.size()) >= kMaxProbes) break;
    const std::string k = key(t.projectId, t.site);
    bool busy = false;
    for (const auto& p : probes_) busy = busy || (p->t.projectId == t.projectId && p->t.site == t.site);
    if (busy) continue;
    auto it = nextAt_.find(k);
    if (it != nextAt_.end() && now < it->second) continue;
    if (t.lastDesc.empty() && !t.localPort) {  // never connected from this computer: nothing to aim at
      PresenceResult& r = results_[t.projectId][t.site];
      if (r.state == PresenceState::Checking) r.state = PresenceState::Unknown;
      nextAt_[k] = now + 60;
      continue;
    }
    startProbe(t, now);
  }
}

}  // namespace gl::collab

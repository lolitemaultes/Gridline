// Gridline - J-111 collaboration: network hub. See Hub.h.
#include "Hub.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Update.h"
#include "Debug.h"

namespace fs = std::filesystem;

namespace gl::collab {

namespace {

constexpr double kMemberRetrySec = 120;  // a member link that did not connect starts again (fresh agent, same port)

std::string keyHex(const crypto::Key& k) { return crypto::toHex(k.data(), 32); }
bool hexKey(const std::string& h, crypto::Key& k) {
  std::string b;
  return crypto::fromHex(h, b) && crypto::keyFrom(b, k);
}
std::string credHex(const crypto::Key& k, size_t chars) { return crypto::toHex(k.data(), 32).substr(0, chars); }

std::string subst(std::string t, const std::string& a, const std::string& b = std::string()) {
  if (const size_t p = t.find("%1"); p != std::string::npos) t.replace(p, 2, a);
  if (const size_t p = t.find("%2"); p != std::string::npos) t.replace(p, 2, b);
  return t;
}

}  // namespace

std::string fmtVersionOlder(const Hub::Refusal& r) {
  const std::string who = r.who.empty() ? std::string(i18n::tr("Your collaborator")) : r.who;
  if (r.sameVersionOtherBuild) return subst(i18n::tr("%1 is on a different build of Gridline %2. Use the same build to collaborate."), who, r.theirVersion);
  return subst(i18n::tr("%1 is on Gridline %2; update to collaborate."), who, r.theirVersion);
}

std::string fmtVersionNewer(const Hub::Refusal& r) {
  const std::string who = r.who.empty() ? std::string(i18n::tr("Your collaborator")) : r.who;
  if (r.sameVersionOtherBuild) return subst(i18n::tr("%1 is on a different build of Gridline %2. Use the same build to collaborate."), who, r.theirVersion);
  return subst(i18n::tr("%1 needs to update Gridline (to %2) before joining."), who, r.myVersion);
}

namespace {

std::string encodeCode(const char* prefix, const Json& j) { return std::string(prefix) + crypto::toB64url(jsonCompact(j)); }

bool decodeCode(const std::string& text, const char* prefix, Json& out) {
  // chat apps wrap long lines and add text around a code: take the longest
  // run of code pieces (split by white space) after the prefix that decodes
  const size_t at = text.find(prefix);
  if (at == std::string::npos) return false;
  std::vector<std::string> pieces;
  std::string cur;
  for (size_t i = at + std::strlen(prefix); i < text.size(); ++i) {
    const char c = text[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!cur.empty()) pieces.push_back(cur);
      cur.clear();
      continue;
    }
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) break;
    cur += c;
  }
  if (!cur.empty()) pieces.push_back(cur);
  for (size_t n = pieces.size(); n > 0; --n) {
    std::string body;
    for (size_t k = 0; k < n; ++k) body += pieces[k];
    std::string raw;
    if (body.size() > 64 * 1024 || !crypto::fromB64url(body, raw)) continue;
    if (jsonParse(raw, out) && out.is(Json::Type::Object)) return true;
  }
  return false;
}

}  // namespace

bool versionsMatch(const VersionInfo& a, const VersionInfo& b) {
  return compareVersions(a.app, b.app) == 0 && a.protocol == b.protocol && a.build == b.build;
}

int compareVersions(const std::string& a, const std::string& b) {
  const update::Version va = update::parseVersion(a), vb = update::parseVersion(b);
  if (!va.valid || !vb.valid) return a == b ? 0 : (a < b ? -1 : 1);
  return update::compare(va, vb);
}

Session::Ticket InviteInfo::ticket() const {
  Session::Ticket t;
  t.projectId = projectId;
  t.projectName = projectName;
  t.creator = creator;
  t.creatorSign = creatorSign;
  return t;
}

std::string makeInviteCode(const InviteInfo& i) {
  Json j = Json::makeObject();
  j.object["v"] = Json::makeNumber(1);
  j.object["pid"] = Json::makeString(i.projectId);
  j.object["pn"] = Json::makeString(i.projectName);
  j.object["cs"] = Json::makeString(siteHex(i.creator));
  j.object["ck"] = Json::makeString(keyHex(i.creatorSign));
  j.object["hs"] = Json::makeString(siteHex(i.inviter));
  j.object["hk"] = Json::makeString(keyHex(i.inviterSign));
  j.object["hd"] = Json::makeString(keyHex(i.inviterDh));
  j.object["hn"] = Json::makeString(i.inviterName);
  j.object["id"] = Json::makeString(crypto::toHex(i.inviteId.data(), i.inviteId.size()));
  j.object["k"] = Json::makeString(keyHex(i.secret));
  j.object["exp"] = Json::makeNumber(static_cast<double>(i.expires));
  j.object["ice"] = Json::makeString(i.ice);
  j.object["av"] = Json::makeString(i.appVersion);
  return encodeCode(kInvitePrefix, j);
}

bool parseInviteCode(const std::string& code, InviteInfo& out, std::string* err) {
  Json j;
  if (!decodeCode(code, kInvitePrefix, j) || j.num("v", 0) != 1) {
    if (err) *err = i18n::tr("This is not a Gridline invite code.");
    return false;
  }
  InviteInfo i;
  i.projectId = j.text("pid");
  i.projectName = cleanProjectName(j.text("pn"));  // shown before joining; the project's own name arrives with it
  i.creator = siteFromHex(j.text("cs"));
  i.inviter = siteFromHex(j.text("hs"));
  i.inviterName = cleanName(j.text("hn"));
  std::string id;
  if (!crypto::fromHex(j.text("id"), id) || id.size() != 16 || !hexKey(j.text("ck"), i.creatorSign) ||
      !hexKey(j.text("hk"), i.inviterSign) || !hexKey(j.text("hd"), i.inviterDh) || !hexKey(j.text("k"), i.secret) ||
      i.projectId.empty() || !i.creator || crypto::siteOf(i.inviterSign) != i.inviter) {
    if (err) *err = i18n::tr("The invite code is damaged. Ask for a new one.");
    return false;
  }
  i.inviteId = id;
  i.expires = static_cast<int64_t>(j.num("exp", 0));
  i.ice = j.text("ice");
  i.appVersion = j.text("av");
  out = std::move(i);
  return true;
}

char codeKind(const std::string& text) {
  if (text.find(kInvitePrefix) != std::string::npos) return 'i';
  if (text.find(kReplyPrefix) != std::string::npos) return 'r';
  if (text.find(kReconnectPrefix) != std::string::npos) return 'c';
  return 0;
}

// ------------------------------------------------------------------- hub ----
Hub::Hub(std::unique_ptr<Fabric> fabric, const crypto::Identity& id, Profile profile, VersionInfo version, std::string stateDir)
    : fabric_(std::move(fabric)), id_(id), profile_(std::move(profile)), version_(std::move(version)), stateDir_(std::move(stateDir)) {
  wallSec = [] { return static_cast<int64_t>(std::time(nullptr)); };
  if (const char* d = std::getenv("GRIDLINE_COLLAB_TEST_ICE_DELAY")) testIceDelaySec = std::max(0.0, std::atof(d));
  // Real networks: transport work runs on its own thread, so links stay alive
  // (keepalives, acks, retransmissions, handshakes) whatever the interface does.
  if (fabric_ && fabric_->real()) startThread();
}

Hub::~Hub() {
  stopThread();
  std::lock_guard<std::recursive_mutex> lk(mu_);
  saveState();
  setSession(nullptr);
}

void Hub::setSession(Session* s) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (session_ == s) return;
  if (session_) {
    saveState();  // addresses and ports for reconnecting next time
    for (auto& [id, l] : links_)
      if (l->sl && l->sl->state() == SecureLink::State::Open) l->sl->close("bye");
    links_.clear();
    invites_.clear();
    via_.clear();
    net_.clear();
    session_->sendFn = nullptr;
    session_->bulkQueuedFn = nullptr;
  }
  session_ = s;
  stateLoaded_ = false;
  join_ = JoinView();
  if (session_) {
    session_->sendFn = [this](SiteId to, int stream, const std::string& wire) { sendTo(to, stream, wire); };
    session_->bulkQueuedFn = [this](SiteId to) -> size_t {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      if (LinkRec* l = directLink(to)) return l->sl->queued(1);
      auto v = via_.find(to);
      if (v != via_.end())
        if (LinkRec* l = directLink(v->second)) return l->sl->queued(1);
      return 0;
    };
    loadState();
  }
}

void Hub::setProfile(const Profile& p) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  profile_ = p;
}

Json Hub::helloJson() const {
  Json h = Json::makeObject();
  h.object["site"] = Json::makeString(siteHex(id_.site()));
  h.object["sign"] = Json::makeString(keyHex(id_.signPublic));
  h.object["app"] = Json::makeString(version_.app);
  h.object["protocol"] = Json::makeNumber(version_.protocol);
  h.object["build"] = Json::makeString(version_.build);
  h.object["profile"] = profileWire(profile_);
  if (session_) h.object["project"] = Json::makeString(session_->projectId());
  return h;
}

bool Hub::checkVersion(const Json& theirs, const std::string& who, Json* refusal) {
  VersionInfo tv;
  tv.app = theirs.text("app");
  tv.protocol = static_cast<int>(theirs.num("protocol", 0));
  tv.build = theirs.text("build");
  if (versionsMatch(tv, version_)) return true;
  Refusal r;
  r.who = who;
  r.theirVersion = tv.app;
  r.myVersion = version_.app;
  const int c = compareVersions(version_.app, tv.app);
  r.sameVersionOtherBuild = c == 0;
  r.weAreOlder = c < 0 || (c == 0 && tv.protocol > version_.protocol);
  if (refusal) {
    refusal->object["reason"] = Json::makeString("version");
    refusal->object["app"] = Json::makeString(version_.app);
    refusal->object["protocol"] = Json::makeNumber(version_.protocol);
    refusal->object["build"] = Json::makeString(version_.build);
    refusal->object["name"] = Json::makeString(cleanName(profile_.name));
  }
  if (onVersionRefusal) onVersionRefusal(r);
  return false;
}

const char* Hub::kindName(int kind) {
  static const char* const kKinds[] = {"invite-host", "invite-join", "member"};
  return kind >= 0 && kind < 3 ? kKinds[kind] : "?";
}

Hub::LinkRec* Hub::newLink(LinkRec::Kind kind, bool initiator, const EndpointConfig& ec0, SecureLink::Hs hs) {
  if (!fabric_) return nullptr;
  EndpointConfig ec = ec0;
  ec.tag = std::string(kindName(static_cast<int>(kind))) + " link " + std::to_string(nextLink_);
  auto ep = fabric_->create(ec);
  if (!ep) {
    debugf("%s: no endpoint", ec.tag.c_str());
    return nullptr;
  }
  debugf("%s created (%s)", ec.tag.c_str(), initiator ? "initiator" : "responder");
  auto l = std::make_unique<LinkRec>();
  l->id = nextLink_++;
  l->kind = kind;
  l->initiator = initiator;
  l->created = now_;
  l->inbox = std::make_shared<Inbox>();
  std::weak_ptr<Inbox> wi = l->inbox;
  std::weak_ptr<Wake> ww = wake_;
  ep->onReceive = [wi, ww](const std::string& d) {
    if (auto in = wi.lock()) {
      std::lock_guard<std::mutex> lk(in->mu);
      if (in->q.size() < 20000) in->q.push_back(d);
    }
    if (auto w = ww.lock()) {  // the network thread handles it now
      {
        std::lock_guard<std::mutex> lk(w->mu);
        w->pending = true;
      }
      w->cv.notify_one();
    }
  };
  l->sl = std::make_unique<SecureLink>(std::move(hs));
  l->sl->tag = ec.tag;
  {
    LinkRec* rec = l.get();  // the record owns the link: the callbacks never outlive it
    l->sl->onMessage = [rec](int st, const std::string& d) { rec->rx.emplace_back(st, d); };
    l->sl->onOpen = [rec] { rec->openedEv = true; };
    l->sl->onClose = [rec](const std::string& why) {
      rec->closedEv = true;
      rec->closeWhy = why;
    };
  }
  Endpoint* raw = ep.get();
  l->sl->output = [raw](const std::string& d) { raw->send(d); };
  l->ep = std::move(ep);
  l->ep->gather();
  LinkRec* p = l.get();
  links_[p->id] = std::move(l);
  return p;
}

void Hub::dropLink(int id, const std::string& why) {
  auto it = links_.find(id);
  if (it == links_.end()) return;
  LinkRec& l = *it->second;
  if (l.up && session_ && l.peer) {
    // another path may still reach them (relay)
    bool other = false;
    for (auto& [k, x] : links_)
      if (k != id && x->up && x->peer == l.peer) other = true;
    if (!other && !via_.count(l.peer)) session_->peerDown(l.peer);
  }
  // One line per closed link, so a dropped connection can be diagnosed from the terminal.
  const std::string line = std::string(kindName(static_cast<int>(l.kind))) + " link " + std::to_string(id) +
                           (l.up ? " closed (" : " abandoned (") + why + (l.error.empty() ? "" : ": " + l.error) + ")";
  std::fprintf(stderr, "collab: %s\n", line.c_str());
  logging::info("collab", line);
  links_.erase(it);
}

Hub::LinkRec* Hub::directLink(SiteId peer) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& [id, l] : links_)
    if (l->up && l->peer == peer && l->sl->state() == SecureLink::State::Open) return l.get();
  return nullptr;
}

crypto::Key Hub::memberPsk(const Member& m) const {
  const crypto::Key shared = crypto::dh(id_.dhSecret, m.dhPublic);
  return crypto::kdf(shared, "gridline-member", session_ ? session_->projectId() : std::string());
}

EndpointConfig Hub::memberEndpoint(SiteId peer, const crypto::Key& psk, bool initiator) {
  EndpointConfig ec;
  ec.ufrag = credHex(crypto::kdf(psk, initiator ? "ufrag-i" : "ufrag-r"), 8);
  ec.pwd = credHex(crypto::kdf(psk, initiator ? "pwd-i" : "pwd-r"), 32);
  ec.remoteUfrag = credHex(crypto::kdf(psk, initiator ? "ufrag-r" : "ufrag-i"), 8);
  ec.remotePwd = credHex(crypto::kdf(psk, initiator ? "pwd-r" : "pwd-i"), 32);
  ec.controlling = initiator;
  ec.localPort = net_[peer].localPort;
  return ec;
}

SecureLink::Hs Hub::memberHs(SiteId peer, bool initiator, const Member& m) {
  SecureLink::Hs hs;
  hs.initiator = initiator;
  hs.psk = memberPsk(m);
  const crypto::Key hid = crypto::kdf(hs.psk, "hsid");
  hs.hsid = std::string(reinterpret_cast<const char*>(hid.data()), 16);
  hs.me = id_;
  hs.remoteDh = m.dhPublic;
  hs.remoteDhKnown = true;
  hs.hello = helloJson();
  // J-111e: a removed member's link only carries the news (they learn it even if they were offline)
  if (session_) {
    const Json n = session_->noticeFor(peer);
    if (!n.is(Json::Type::Null)) hs.hello.object["notice"] = n;
  }
  const std::string who = m.name;
  hs.accept = [this, peer, who](const Json& theirs, const crypto::Key&, bool& ok) {
    Json reply = helloJson();
    if (session_ && session_->stopped()) {  // removed / ended / left here: no links any more
      if (const Json* n = theirs.find("notice")) session_->learnNotice(*n);
      ok = false;
      reply.object["reason"] = Json::makeString("noticed");
      return reply;
    }
    if (const Json* n = theirs.find("notice"); n && session_ && siteFromHex(theirs.text("site")) == peer && session_->learnNotice(*n)) {
      ok = false;  // we were removed / the project ended: understood
      reply.object["reason"] = Json::makeString("noticed");
      return reply;
    }
    if (session_) {
      const Json n = session_->noticeFor(peer);
      if (!n.is(Json::Type::Null)) {
        ok = false;
        for (const auto& [k, v] : n.object) reply.object[k] = v;
        return reply;
      }
    }
    ok = siteFromHex(theirs.text("site")) == peer && checkVersion(theirs, who, &reply);
    return reply;
  };
  return hs;
}

void Hub::startMemberLink(SiteId peer, bool initiator, const std::string& remoteDesc) {
  if (!session_) return;
  const Member* mp = session_->member(peer);
  if (!mp) return;
  const Member m = *mp;
  // one link attempt per peer
  for (auto& [id, l] : links_)
    if (l->kind == LinkRec::Kind::Member && l->peer == peer && !l->dead) {
      if (!remoteDesc.empty() && l->remoteDesc != remoteDesc) {
        l->remoteDesc = remoteDesc;
        l->ep->setRemote(remoteDesc);
      }
      return;
    }
  const crypto::Key psk = memberPsk(m);
  LinkRec* l = newLink(LinkRec::Kind::Member, initiator, memberEndpoint(peer, psk, initiator), memberHs(peer, initiator, m));
  if (!l) return;
  l->peer = peer;
  if (!remoteDesc.empty()) {
    l->remoteDesc = remoteDesc;
    l->ep->setRemote(remoteDesc);
  }
}

// ---------------------------------------------------------------- invites ----
bool Hub::shunned(SiteId peer) const {
  if (!session_) return false;
  const MemberState st = session_->memberState(peer);
  return st == MemberState::Removed || st == MemberState::Left;
}

bool Hub::isNotice(const std::string& wire) {
  Message m;
  return Message::decode(wire, m) && m.type() == "notice";
}

std::string Hub::createInvite(int minutes, SiteId readmit) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!session_ || !fabric_ || session_->stopped() || session_->ended()) return {};
  InviteRec r;
  r.readmit = readmit;
  r.info.projectId = session_->projectId();
  r.info.projectName = session_->projectName();
  r.info.creator = session_->creator();
  if (const Member* c = session_->member(session_->creator())) r.info.creatorSign = c->signPublic;
  else r.info.creatorSign = id_.signPublic;
  r.info.inviter = id_.site();
  r.info.inviterSign = id_.signPublic;
  r.info.inviterDh = id_.dhPublic;
  r.info.inviterName = cleanName(profile_.name);
  std::string iid(16, '\0');
  crypto::randomBytes(&iid[0], 16);
  r.info.inviteId = iid;
  r.info.secret = crypto::randomKey();
  r.info.expires = wallSec() + std::max(1, minutes) * 60;
  r.info.appVersion = version_.app;
  r.view.id = crypto::toHex(iid.data(), 16);
  r.view.expires = r.info.expires;
  EndpointConfig ec;
  ec.ufrag = credHex(crypto::randomKey(), 8);
  ec.pwd = credHex(crypto::randomKey(), 32);
  ec.remoteUfrag = credHex(crypto::kdf(r.info.secret, "ufrag-j"), 8);
  ec.remotePwd = credHex(crypto::kdf(r.info.secret, "pwd-j"), 32);
  SecureLink::Hs hs;
  hs.initiator = false;
  hs.psk = r.info.secret;
  hs.hsid = iid;
  hs.me = id_;
  hs.hello = helloJson();
  const std::string inviteKey = r.view.id;
  hs.accept = [this, inviteKey](const Json& theirs, const crypto::Key& theirDh, bool& ok) {
    Json reply = helloJson();
    auto it = invites_.find(inviteKey);
    const std::string who = cleanName(theirs.find("profile") ? theirs.find("profile")->text("name") : std::string());
    if (it == invites_.end() || it->second.used || it->second.info.expired(wallSec())) {
      ok = false;
      reply.object["reason"] = Json::makeString(it == invites_.end() || it->second.used ? "used" : "expired");
      return reply;
    }
    if (!checkVersion(theirs, who.empty() ? i18n::tr("The invitee") : who, &reply)) {
      ok = false;
      return reply;
    }
    crypto::Key sign{};
    if (!hexKey(theirs.text("sign"), sign) || crypto::siteOf(sign) != siteFromHex(theirs.text("site"))) {
      ok = false;
      reply.object["reason"] = Json::makeString("identity");
      return reply;
    }
    if (session_ && session_->ended()) {  // stopped sharing: nobody joins any more
      ok = false;
      const Json n = session_->noticeFor(0);
      for (const auto& [k, v] : n.object) reply.object[k] = v;
      return reply;
    }
    if (session_ && session_->memberState(crypto::siteOf(sign)) == MemberState::Removed) {
      // removed: only the admin's re-invite of exactly them brings them back
      if (it->second.readmit == crypto::siteOf(sign) && session_->isAdmin()) {
        session_->readmit(crypto::siteOf(sign));
      } else {
        ok = false;
        const Json n = session_->noticeFor(crypto::siteOf(sign));
        for (const auto& [k, v] : n.object) reply.object[k] = v;
        return reply;
      }
    }
    it->second.used = true;
    // admit the new member now, so its first sync carries the record
    Member m;
    m.site = crypto::siteOf(sign);
    m.signPublic = sign;
    m.dhPublic = theirDh;
    m.name = who;
    const int slot = session_ ? session_->admitMember(m) : 0;
    reply.object["slot"] = Json::makeNumber(slot);
    ok = true;
    return reply;
  };
  LinkRec* l = newLink(LinkRec::Kind::InviteHost, false, ec, std::move(hs));
  if (!l) {
    r.view.status = InviteStatus::Failed;
    r.view.error = i18n::tr("Networking is not available in this build.");
  } else {
    l->inviteId = r.view.id;
    r.link = l->id;
  }
  invites_[r.view.id] = std::move(r);
  return inviteKey;
}

const Hub::InviteView* Hub::invite(const std::string& id) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto it = invites_.find(id);
  return it == invites_.end() ? nullptr : &it->second.view;
}

std::vector<Hub::InviteView> Hub::invites() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::vector<InviteView> v;
  for (const auto& [k, r] : invites_) v.push_back(r.view);
  return v;
}

void Hub::cancelInvite(const std::string& id) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto it = invites_.find(id);
  if (it == invites_.end()) return;
  if (it->second.view.status != InviteStatus::Connected) dropLink(it->second.link, "cancelled");
  invites_.erase(it);
}

bool Hub::acceptReply(const std::string& code, std::string* err) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Json j;
  if (!decodeCode(code, kReplyPrefix, j)) {
    if (err) *err = i18n::tr("This is not a Gridline reply code.");
    return false;
  }
  auto it = invites_.find(j.text("id"));
  if (it == invites_.end()) {
    if (err) *err = i18n::tr("This reply belongs to another invite.");
    return false;
  }
  auto l = links_.find(it->second.link);
  if (l == links_.end()) {
    if (err) *err = i18n::tr("This invite is no longer open.");
    return false;
  }
  l->second->remoteDesc = j.text("ice");
  l->second->ep->setRemote(l->second->remoteDesc);
  return true;
}

// ------------------------------------------------------------------ join ----
bool Hub::join(const InviteInfo& info, std::string* err) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!session_) return false;
  if (info.expired(wallSec())) {
    if (err) *err = i18n::tr("This invite has expired. Ask for a new one.");
    return false;
  }
  joinInfo_ = info;
  EndpointConfig ec;
  ec.ufrag = credHex(crypto::kdf(info.secret, "ufrag-j"), 8);
  ec.pwd = credHex(crypto::kdf(info.secret, "pwd-j"), 32);
  ec.controlling = true;
  SecureLink::Hs hs;
  hs.initiator = true;
  hs.psk = info.secret;
  hs.hsid = info.inviteId;
  hs.me = id_;
  hs.remoteDh = info.inviterDh;
  hs.remoteDhKnown = true;
  hs.hello = helloJson();
  LinkRec* l = newLink(LinkRec::Kind::InviteJoin, true, ec, std::move(hs));
  if (!l) {
    if (err) *err = i18n::tr("Networking is not available in this build.");
    return false;
  }
  l->peer = info.inviter;
  if (testIceDelaySec > 0) {
    l->heldDesc = info.ice;
    l->heldUntil = now_ + testIceDelaySec;
  } else {
    l->ep->setRemote(info.ice);
  }
  joinLink_ = l->id;
  join_ = JoinView();
  join_.status = JoinStatus::Connecting;
  join_.since = now_;
  // the inviter is a member already (its record arrives with the first sync)
  PeerNet& pn = net_[info.inviter];
  pn.lastDesc = info.ice;
  pn.name = info.inviterName;
  return true;
}

// ------------------------------------------------------------- reconnect ----
std::string Hub::reconnectCode(SiteId peer) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!session_) return {};
  const Member* m = session_->member(peer);
  if (!m || !session_->isActive(peer)) return {};
  const bool initiator = id_.site() < peer;
  startMemberLink(peer, initiator, net_[peer].lastDesc);
  for (auto& [id, l] : links_)
    if (l->kind == LinkRec::Kind::Member && l->peer == peer && !l->dead && l->ep->gathered()) {
      Json j = Json::makeObject();
      j.object["v"] = Json::makeNumber(1);
      j.object["pid"] = Json::makeString(session_->projectId());
      j.object["from"] = Json::makeString(siteHex(id_.site()));
      j.object["to"] = Json::makeString(siteHex(peer));
      j.object["ice"] = Json::makeString(l->ep->localDescription());
      return encodeCode(kReconnectPrefix, j);
    }
  return {};  // still gathering: ask again in a moment
}

bool Hub::acceptReconnectCode(const std::string& code, std::string* err) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Json j;
  if (!decodeCode(code, kReconnectPrefix, j)) {
    if (err) *err = i18n::tr("This is not a Gridline reconnect code.");
    return false;
  }
  if (!session_ || j.text("pid") != session_->projectId()) {
    if (err) *err = i18n::tr("This code is for another project.");
    return false;
  }
  const SiteId from = siteFromHex(j.text("from"));
  if (!session_->isActive(from) || from == id_.site()) {
    if (err) *err = i18n::tr("This code is from someone who is not a member of this project.");
    return false;
  }
  net_[from].lastDesc = j.text("ice");
  net_[from].failures = 0;
  startMemberLink(from, id_.site() < from, j.text("ice"));
  return true;
}

// ---------------------------------------------------------------- traffic ----
void Hub::sendNet(SiteId to, const Message& m) {
  if (LinkRec* l = directLink(to)) l->sl->send(kStreamControl, m.encode());
}

void Hub::sendTo(SiteId to, int stream, const std::string& wire) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (shunned(to) && !isNotice(wire)) return;  // only the news of their removal still goes to them
  if (LinkRec* l = directLink(to)) {
    l->sl->send(stream, wire);
    return;
  }
  auto v = via_.find(to);
  if (v == via_.end()) return;
  LinkRec* r = directLink(v->second);
  if (!r) return;
  Message f;
  f.head.object["m"] = Json::makeString("net.fwd");
  f.head.object["to"] = Json::makeString(siteHex(to));
  f.head.object["from"] = Json::makeString(siteHex(id_.site()));
  f.head.object["s"] = Json::makeNumber(stream);
  f.bin = wire;
  const std::string w = f.encode();
  if (stream == kStreamUnreliable && w.size() < kMaxPacket - 60) r->sl->send(kStreamUnreliable, w);
  else r->sl->send(stream == kStreamUnreliable ? kStreamControl : stream, w);
}

void Hub::onLinkOpen(LinkRec& l) {
  const Json& h = l.sl->remoteHello();
  const SiteId peer = siteFromHex(h.text("site"));
  if (!peer || peer == id_.site()) {
    l.dead = true;
    return;
  }
  l.peer = peer;
  l.up = true;
  debugf("%s open with %s (path %s)", l.sl->tag.c_str(), siteHex(peer).c_str(), l.ep->path().c_str());
  PeerNet& pn = net_[peer];
  pn.failures = 0;
  pn.backoff = 10;
  if (!l.remoteDesc.empty()) pn.lastDesc = l.remoteDesc;
  if (const uint16_t port = l.ep->localPort()) pn.localPort = port;
  if (const Json* p = h.find("profile")) pn.name = p->text("name");
  if (l.kind == LinkRec::Kind::InviteHost) {
    auto it = invites_.find(l.inviteId);
    if (it != invites_.end()) {
      it->second.view.status = InviteStatus::Connected;
      it->second.view.joinerName = pn.name;
      it->second.view.safety = l.sl->safetyCode();
    }
  }
  if (l.kind == LinkRec::Kind::InviteJoin) {
    join_.status = JoinStatus::Connected;
    join_.safety = l.sl->safetyCode();
  }
  // our description for this link: its port is the one we listen on for them
  // next time, so they can reconnect after a restart without a code
  {
    Message d;
    d.head.object["m"] = Json::makeString("net.desc");
    d.head.object["ice"] = Json::makeString(l.ep->localDescription());
    l.sl->send(kStreamControl, d.encode());
  }
  // a direct link replaces a relayed path
  const bool wasRelayed = via_.erase(peer) > 0;
  if (session_ && !wasRelayed) session_->peerUp(peer);
  saveState();
}

void Hub::handleNet(SiteId from, const Message& m) {
  if (shunned(from)) return;  // a removed or departed member: nothing relayed, introduced or forwarded
  const std::string type = m.type();
  if (type == "net.peers") {
    PeerNet& pn = net_[from];
    pn.theirLinks.clear();
    if (const Json* l = m.head.find("l"); l && l->is(Json::Type::Array))
      for (const auto& s : l->array) pn.theirLinks.insert(siteFromHex(s.str));
    pn.peersAt = now_;
  } else if (type == "net.fwd") {
    const SiteId to = siteFromHex(m.head.text("to")), origin = siteFromHex(m.head.text("from"));
    const int s = static_cast<int>(m.head.num("s", 0));
    if (shunned(origin) || (shunned(to) && !isNotice(m.bin))) return;
    if (to == id_.site()) {
      if (!session_ || !session_->member(origin)) return;
      Message inner;
      if (Message::decode(m.bin, inner) && inner.type().rfind("net.", 0) == 0) {
        handleNet(origin, inner);  // signalling through the relay
        return;
      }
      if (!via_.count(origin) && !directLink(origin)) {
        via_[origin] = from;
        session_->peerUp(origin);
      }
      session_->receive(origin, s, m.bin);
    } else if (LinkRec* l = directLink(to)) {
      // we are the collaborator in between: pass it on (we cannot read or change signed ops)
      l->sl->send(s == kStreamUnreliable ? kStreamControl : s, m.encode());
    }
  } else if (type == "net.sig") {
    // a member reachable through someone else sends its ICE description: try a direct link
    const std::string desc = m.head.text("ice");
    if (!session_ || !session_->member(from) || desc.empty()) return;
    net_[from].lastDesc = desc;
    startMemberLink(from, id_.site() < from, desc);
  } else if (type == "net.desc") {
    // their addresses (and the port they will listen on for us): reconnects after a restart
    const std::string desc = m.head.text("ice");
    if (!desc.empty() && desc.find("a=candidate:") != std::string::npos && desc.size() < 8192) {
      net_[from].lastDesc = desc;
      saveState();
    }
  } else if (type == "net.bye") {
    for (auto& [id, l] : links_)
      if (l->peer == from) l->dead = true;
  }
}

void Hub::onLinkMessage(LinkRec& l, int stream, const std::string& data) {
  if (!l.up) return;
  Message m;
  if (!Message::decode(data, m)) return;
  if (m.type().rfind("net.", 0) == 0) {
    handleNet(l.peer, m);
    return;
  }
  if (session_) session_->receive(l.peer, stream, data);
}

void Hub::relayRoutes() {
  if (!session_) return;
  // who reaches whom: relay through a directly connected collaborator
  std::set<SiteId> direct;
  for (auto& [id, l] : links_)
    if (l->up && l->sl->state() == SecureLink::State::Open) direct.insert(l->peer);
  for (auto it = via_.begin(); it != via_.end();) {  // nobody relays for (or through) a removed member
    if (shunned(it->first) || shunned(it->second)) {
      session_->peerDown(it->first);
      it = via_.erase(it);
    } else {
      ++it;
    }
  }
  for (const Member& m : session_->activeMembers()) {
    const SiteId s = m.site;
    if (s == id_.site() || direct.count(s)) continue;
    SiteId best = 0;
    for (SiteId c : direct) {
      auto it = net_.find(c);
      if (it != net_.end() && it->second.theirLinks.count(s) && now_ - it->second.peersAt < 15) best = c;
    }
    auto cur = via_.find(s);
    if (best) {
      if (cur == via_.end()) {
        via_[s] = best;
        session_->peerUp(s);
        // offer a direct link through the relay (no code needed)
        if (id_.site() < s) startMemberLink(s, true, net_[s].lastDesc);
      } else {
        cur->second = best;
      }
    } else if (cur != via_.end()) {
      via_.erase(cur);
      session_->peerDown(s);
    }
  }
}

void Hub::autoReconnect() {
  if (!session_ || !session_->ready() || session_->stopped()) return;
  for (const Member& m : session_->members()) {
    const SiteId s = m.site;
    if (s == id_.site() || directLink(s)) continue;
    const MemberState st = session_->memberState(s);
    if (st == MemberState::Left) continue;
    // removed: a link now and then only to tell them (it never opens), until they know
    if (st == MemberState::Removed && net_[s].notified) continue;
    bool pending = false;
    for (auto& [id, l] : links_)
      if (l->peer == s && !l->dead) pending = true;
    if (pending) continue;
    PeerNet& pn = net_[s];
    if (now_ < pn.nextTry) continue;
    // Nothing known about them (never connected from this computer): a reconnect
    // code or a collaborator in between is needed. Otherwise a link always waits
    // for them on the port they know (their checks find it even when we know no
    // address of theirs) and tries their last addresses.
    if (pn.lastDesc.empty() && !pn.localPort && !via_.count(s)) continue;
    pn.nextTry = now_ + 2;
    debugf("member link to %s: %s (attempt %d)", siteHex(s).c_str(), pn.lastDesc.empty() ? "waiting for them" : "trying their last addresses",
           pn.failures + 1);
    if (st == MemberState::Removed) pn.nextTry = now_ + 60;
    startMemberLink(s, id_.site() < s, pn.lastDesc);
  }
}

double Hub::clockSec() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - born_).count();
}

void Hub::startThread() {
  if (thread_.joinable()) return;
  threaded_ = true;
  stop_ = false;
  thread_ = std::thread([this] {
    std::unique_lock<std::mutex> wl(wake_->mu);
    while (!stop_) {
      wake_->cv.wait_for(wl, std::chrono::milliseconds(10), [this] { return stop_.load() || wake_->pending; });
      wake_->pending = false;
      wl.unlock();
      {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (!stop_) transportStep(clockSec());
      }
      wl.lock();
    }
  });
}

void Hub::stopThread() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> wl(wake_->mu);
    stop_ = true;
  }
  wake_->cv.notify_all();
  thread_.join();
}

// Transport work for every link: datagrams in, handshakes, acks, keepalives,
// retransmissions. On the network thread (real networks) or inside poll (tests).
// Nothing here calls into the Session: messages, opened / closed links and a
// responder's handshake packets (whose acceptance admits members) wait in the
// link record for poll on the UI thread.
void Hub::transportStep(double t) {
  if (t > now_) now_ = t;
  for (auto& [id, lp] : links_) {
    LinkRec& l = *lp;
    if (l.dead) continue;
    if (l.heldUntil >= 0 && now_ >= l.heldUntil) {
      debugf("%s test: the remote candidates arrive now", l.sl->tag.c_str());
      l.heldUntil = -1;
      l.ep->setRemote(l.heldDesc);
    }
    std::deque<std::string> in;
    {
      std::lock_guard<std::mutex> lk(l.inbox->mu);
      in.swap(l.inbox->q);
    }
    for (auto& d : in) {
      if (!l.initiator && l.sl->state() == SecureLink::State::Handshake) l.uiIn.push_back(std::move(d));
      else l.sl->input(d, nowMs());
    }
    if (l.iceAt < 0 && l.ep->state() == Endpoint::State::Connected) l.iceAt = now_;
    if (!l.started && l.initiator && l.ep->state() == Endpoint::State::Connected) {
      debugf("%s ICE connected after %.2f s: handshake starts", l.sl->tag.c_str(), now_ - l.created);
      l.started = true;
      l.startedAt = now_;
      l.sl->start(nowMs());
    }
    if (!l.started && !l.initiator) {
      l.started = true;
      l.startedAt = now_;
      l.sl->start(nowMs());
    }
    l.sl->tick(nowMs());
  }
}

void Hub::poll(double nowSec) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (threaded_) {
    const double t = clockSec();
    if (lastPoll_ >= 0 && t - lastPoll_ > 1.0 && !links_.empty())
      debugf("poll gap %.2f s (the interface paused; the network thread kept the links alive)", t - lastPoll_);
    lastPoll_ = t;
    if (t > now_) now_ = t;
  } else {
    if (now_ > 0 && nowSec - now_ > 0.25 && !links_.empty()) debugf("poll gap %.2f s (the network waited for a frame)", nowSec - now_);
    transportStep(nowSec);
  }
  // J-111e: links to members who were removed or left close at once
  if (session_)
    for (auto& [id, l] : links_)
      if (l->up && !l->dead && l->peer && shunned(l->peer)) {
        auto it = shunnedSince_.find(l->peer);
        if (it == shunnedSince_.end()) it = shunnedSince_.emplace(l->peer, now_).first;
        if (now_ - it->second < 2.0) continue;  // the notice is on its way
        if (l->sl->state() == SecureLink::State::Open) l->sl->close("removed");
        l->dead = true;
      }
  // events of the links (UI thread): handshakes to accept, opened links, messages
  std::vector<int> ids;
  for (auto& [id, l] : links_) ids.push_back(id);
  for (int id : ids) {
    auto it = links_.find(id);
    if (it == links_.end()) continue;
    LinkRec& l = *it->second;
    if (!l.uiIn.empty()) {
      std::vector<std::string> in;
      in.swap(l.uiIn);
      for (const auto& d : in) l.sl->input(d, nowMs());
    }
    const bool opened = l.openedEv;
    l.openedEv = false;
    if (opened || (l.sl->state() == SecureLink::State::Open && !l.up)) onLinkOpen(l);
    std::vector<std::pair<int, std::string>> msgs;
    msgs.swap(l.rx);
    for (const auto& [st, d] : msgs) {
      if (links_.find(id) == links_.end()) break;
      onLinkMessage(l, st, d);
    }
    if (links_.find(id) == links_.end()) continue;
    const bool closed = l.closedEv;
    const std::string closeWhy = l.closeWhy;
    l.closedEv = false;
    if (l.sl->state() == SecureLink::State::Rejected) {
      const Json& r = l.sl->refusal();
      const std::string reason = r.text("reason");
      std::string msg;
      if (reason == "version") {
        VersionInfo tv;
        tv.app = r.text("app");
        tv.protocol = static_cast<int>(r.num("protocol", 0));
        tv.build = r.text("build");
        Refusal rf;
        rf.who = cleanName(r.text("name"));
        rf.theirVersion = tv.app;
        rf.myVersion = version_.app;
        const int c = compareVersions(version_.app, tv.app);
        rf.sameVersionOtherBuild = c == 0;
        rf.weAreOlder = c < 0 || (c == 0 && tv.protocol > version_.protocol);
        if (l.initiator && onVersionRefusal) onVersionRefusal(rf);
        msg = rf.weAreOlder ? fmtVersionOlder(rf) : fmtVersionNewer(rf);
      } else if (reason == "expired") {
        msg = i18n::tr("This invite has expired. Ask for a new one.");
      } else if (reason == "used") {
        msg = i18n::tr("This invite was already used. Ask for a new one.");
      } else if (reason == "removed" || reason == "ended") {
        // J-111e: we were removed / the project ended while we were away
        if (session_) session_->learnNotice(r);
        const std::string by = cleanName(r.text("byName"));
        msg = reason == "removed" ? subst(i18n::tr("You were removed from this project by %1. Only a new invite from an admin brings you back."), by)
                                  : subst(i18n::tr("%1 stopped sharing this project."), by);
      } else if (reason == "noticed") {
        net_[l.peer].notified = true;
        msg = i18n::tr("The connection was refused.");
      } else {
        msg = i18n::tr("The connection was refused.");
      }
      if (l.kind == LinkRec::Kind::InviteJoin) {
        join_.status = JoinStatus::Refused;
        join_.message = msg;
      }
      l.dead = true;
    }
    if (closed && l.up) {
      l.dead = true;
      l.error = closeWhy;
    }
    // Links that never open: ICE gets 45 s for a join (2 min for a member link,
    // which then starts again at once with a fresh agent on the same port, so a
    // collaborator who comes online finds it), the handshake 35 s after ICE.
    const double age = now_ - l.created;
    const bool handshakeOver = l.started && l.sl->state() != SecureLink::State::Handshake && l.sl->state() != SecureLink::State::Open;
    const double iceLimit = l.kind == LinkRec::Kind::Member ? kMemberRetrySec : 45.0;
    const bool tooLong = handshakeOver || (l.iceAt < 0 ? age > iceLimit : now_ - l.iceAt > 35);
    if (!l.up && !l.dead && l.kind != LinkRec::Kind::InviteHost && l.sl->state() != SecureLink::State::Open && tooLong) {
      l.dead = true;
      PeerNet& pn = net_[l.peer];
      ++pn.failures;
      debugf("%s gave up after %.0f s (ice %d, handshake %d)", l.sl->tag.c_str(), age, static_cast<int>(l.ep->state()), static_cast<int>(l.sl->state()));
      if (l.kind == LinkRec::Kind::InviteJoin) {
        join_.status = JoinStatus::Failed;
        join_.message = i18n::tr("Couldn't connect directly. Your network or theirs blocks peer-to-peer connections.");
      }
    }
    if (l.kind == LinkRec::Kind::InviteJoin && join_.status == JoinStatus::Connecting && age > 5 && l.ep->gathered()) {
      join_.status = JoinStatus::NeedReply;
      Json j = Json::makeObject();
      j.object["v"] = Json::makeNumber(1);
      j.object["id"] = Json::makeString(crypto::toHex(joinInfo_.inviteId.data(), 16));
      j.object["ice"] = Json::makeString(l.ep->localDescription());
      join_.reply = encodeCode(kReplyPrefix, j);
      debugf("%s not connected after %.1f s: reply code offered", l.sl->tag.c_str(), age);
    }
    // member links that learnt no candidates: send ours through a relay once gathered
    if (l.kind == LinkRec::Kind::Member && !l.up && !l.sigSent && l.ep->gathered() && via_.count(l.peer)) {
      l.sigSent = true;
      Message sig;
      sig.head.object["m"] = Json::makeString("net.sig");
      sig.head.object["ice"] = Json::makeString(l.ep->localDescription());
      sendTo(l.peer, kStreamControl, sig.encode());
    }
  }
  for (auto it = links_.begin(); it != links_.end();) {
    if (it->second->dead) {
      const int id = it->first;
      ++it;
      dropLink(id, "dead");
    } else {
      ++it;
    }
  }
  // invites: code ready once candidates (and the router mapping, or 3 s) are in
  const int64_t wall = wallSec();
  for (auto& [k, r] : invites_) {
    auto l = links_.find(r.link);
    if (r.view.status == InviteStatus::Preparing && l != links_.end()) {
      if (l->second->ep->gathered() || (r.gatheredAt >= 0 && now_ - r.gatheredAt > 3)) {
        r.info.ice = l->second->ep->localDescription();
        r.view.code = makeInviteCode(r.info);
        r.view.status = InviteStatus::Ready;
        debugf("invite %s ready after %.2f s (%s)", k.substr(0, 8).c_str(), now_ - l->second->created,
               l->second->ep->gathered() ? "gathered" : "gathering timed out");
      } else if (r.gatheredAt < 0 && now_ - l->second->created > 2) {
        r.gatheredAt = now_;
      }
    }
    if (r.view.status == InviteStatus::Ready && l != links_.end() && l->second->ep->state() == Endpoint::State::Connected)
      r.view.status = InviteStatus::Connecting;
    if ((r.view.status == InviteStatus::Ready || r.view.status == InviteStatus::Preparing || r.view.status == InviteStatus::Connecting) &&
        r.info.expired(wall)) {
      r.view.status = InviteStatus::Expired;
      dropLink(r.link, "expired");
    }
  }
  // tell directly connected members whom we reach (relay and introductions)
  if (session_ && (lastPeers_ < 0 || now_ - lastPeers_ >= 3)) {
    lastPeers_ = now_;
    Message m;
    m.head.object["m"] = Json::makeString("net.peers");
    Json list = Json::makeArray();
    for (auto& [id, l] : links_)
      if (l->up) list.array.push_back(Json::makeString(siteHex(l->peer)));
    m.head.object["l"] = std::move(list);
    const std::string w = m.encode();
    for (auto& [id, l] : links_)
      if (l->up && l->sl->state() == SecureLink::State::Open) {
        l->sl->send(kStreamControl, w);
        if (session_) session_->setRtt(l->peer, l->sl->rttMs());
      }
    relayRoutes();
    autoReconnect();
  }
  if (lastSave_ < 0 || now_ - lastSave_ > 30) {
    lastSave_ = now_;
    saveState();
  }
}

std::vector<Hub::LinkView> Hub::links() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::vector<LinkView> out;
  if (!session_) return out;
  for (const Member& m : session_->activeMembers())
    if (m.site != id_.site()) out.push_back(link(m.site));
  return out;
}

Hub::LinkView Hub::link(SiteId site) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  LinkView v;
  v.site = site;
  for (const auto& [id, l] : links_) {
    if (l->peer != site) continue;
    if (l->up && l->sl->state() == SecureLink::State::Open) {
      v.state = LinkView::State::Direct;
      v.path = l->ep->path();
      v.rttMs = l->sl->rttMs();
      return v;
    }
    if (!l->dead) v.state = LinkView::State::Connecting;
  }
  auto known = net_.find(site);
  if (v.state == LinkView::State::Connecting && known != net_.end()) {
    v.attempts = known->second.failures;
    // a member link that has not connected for a while: they are offline (or moved)
    for (const auto& [id, l] : links_)
      if (l->peer == site && !l->dead && l->kind == LinkRec::Kind::Member && (known->second.failures > 0 || now_ - l->created > 20))
        v.state = LinkView::State::Waiting;
  }
  auto r = via_.find(site);
  if (r != via_.end()) {
    v.state = LinkView::State::Relayed;
    auto n = net_.find(r->second);
    v.via = n != net_.end() ? n->second.name : std::string();
    return v;
  }
  auto pn = net_.find(site);
  if (v.state == LinkView::State::None && pn != net_.end() && pn->second.failures > 0) {
    v.state = LinkView::State::Failed;
    v.error = i18n::tr("Couldn't connect directly. Your network or theirs blocks peer-to-peer connections.");
  }
  return v;
}

std::string Hub::describeLinks() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  static const char* const kEp[] = {"new", "gathering", "connecting", "connected", "failed"};
  static const char* const kSl[] = {"handshake", "open", "closed", "rejected"};
  std::string out;
  for (const auto& [id, l] : links_) {
    char b[320];
    const int es = static_cast<int>(l->ep->state()), ss = static_cast<int>(l->sl->state());
    std::snprintf(b, sizeof b, "link %d %s peer %s ice %s path %s handshake %s up %d dead %d age %.1f s heard %.1f s ago port %u\n", id,
                  kindName(static_cast<int>(l->kind)), siteHex(l->peer).c_str(), es >= 0 && es < 5 ? kEp[es] : "?", l->ep->path().c_str(),
                  ss >= 0 && ss < 4 ? kSl[ss] : "?", l->up ? 1 : 0, l->dead ? 1 : 0, now_ - l->created,
                  l->sl->lastHeardMs() ? now_ - static_cast<double>(l->sl->lastHeardMs()) / 1000.0 : -1.0, static_cast<unsigned>(l->ep->localPort()));
    out += b;
  }
  return out;
}

// ------------------------------------------------------------------ state ----
void Hub::loadState() {
  stateLoaded_ = true;
  if (!session_ || stateDir_.empty()) return;
  std::string text;
  Json j;
  if (!readFileText((fs::path(stateDir_) / (session_->projectId() + ".json")).string(), text, 1 << 20) || !jsonParse(text, j)) return;
  if (const Json* peers = j.find("peers"); peers && peers->is(Json::Type::Object))
    for (const auto& [k, v] : peers->object) {
      PeerNet& pn = net_[siteFromHex(k)];
      pn.lastDesc = v.text("desc");
      pn.localPort = static_cast<uint16_t>(v.num("port", 0));
      pn.name = v.text("name");
    }
}

void Hub::saveState() {
  if (!session_ || stateDir_.empty() || session_->projectId().empty()) return;
  Json j = Json::makeObject();
  Json peers = Json::makeObject();
  for (const auto& [s, pn] : net_) {
    if (!s || (pn.lastDesc.empty() && !pn.localPort)) continue;
    Json p = Json::makeObject();
    p.object["desc"] = Json::makeString(pn.lastDesc);
    p.object["port"] = Json::makeNumber(pn.localPort);
    p.object["name"] = Json::makeString(pn.name);
    peers.object[siteHex(s)] = std::move(p);
  }
  j.object["peers"] = std::move(peers);
  std::error_code ec;
  fs::create_directories(stateDir_, ec);
  writeFileDurable((fs::path(stateDir_) / (session_->projectId() + ".json")).string(), jsonStringify(j, 2));
}

}  // namespace gl::collab

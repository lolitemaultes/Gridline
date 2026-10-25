// Gridline - J-111 collaboration: the network side of a session.
//
// Codes (no server): an invite code carries the project, the inviter's keys,
// a one-time secret, an expiry and the inviter's ICE description; the
// invitee's ICE credentials are derived from the secret, so on the same
// network or with a reachable inviter no reply is needed; otherwise the
// invitee sends back a reply code (its candidates). Reconnect codes do the
// same between members (authenticated by their static keys).
//
// Paths, in order: direct ICE (host / IPv6 / hole punching via STUN), router
// port mapping (libplum), relay through another collaborator who reaches both
// (store-and-forward of signed, end-to-end encrypted traffic), a TURN server
// from the settings. Members who are both online through a third one also
// exchange ICE descriptions through it and connect directly without codes.
//
// The handshake exchanges app version, protocol version and build id: peers
// on different builds are refused politely (the older one is told to update).
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Link.h"
#include "Profile.h"
#include "Session.h"
#include "Transport.h"

namespace gl::collab {

struct VersionInfo {
  std::string app;              // "0.7.0"
  int protocol = kProtocolVersion;
  std::string build;            // build id (commit), "" = unknown
};
// Same version, protocol and build: may collaborate.
bool versionsMatch(const VersionInfo& a, const VersionInfo& b);
// < 0: a is older than b.
int compareVersions(const std::string& a, const std::string& b);

struct InviteInfo {
  std::string projectId, projectName;
  SiteId creator = 0;
  crypto::Key creatorSign{};
  SiteId inviter = 0;
  crypto::Key inviterSign{}, inviterDh{};
  std::string inviterName;
  std::string inviteId;   // 16 bytes
  crypto::Key secret{};
  int64_t expires = 0;    // unix seconds
  std::string ice;        // the inviter's ICE description
  std::string appVersion;
  bool expired(int64_t nowSec) const { return nowSec > expires; }
  std::string inviterFingerprint() const { return crypto::fingerprintOf(inviterSign); }
  Session::Ticket ticket() const;
};
inline constexpr const char* kInvitePrefix = "gridline-invite:";
inline constexpr const char* kReplyPrefix = "gridline-reply:";
inline constexpr const char* kReconnectPrefix = "gridline-reconnect:";
std::string makeInviteCode(const InviteInfo& i);
bool parseInviteCode(const std::string& code, InviteInfo& out, std::string* err = nullptr);
// What kind of code a pasted text is: 'i' invite, 'r' reply, 'c' reconnect, 0 none.
char codeKind(const std::string& text);

class Hub {
 public:
  Hub(std::unique_ptr<Fabric> fabric, const crypto::Identity& id, Profile profile, VersionInfo version, std::string stateDir);
  ~Hub();
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  // The open collaborative session (nullptr: none; closes every link).
  void setSession(Session* s);
  Session* session() const { return session_; }
  void setProfile(const Profile& p);
  // Per frame (UI thread): messages for the session, opened and closed links,
  // invites, reconnects, relay. With a real network (libjuice) the transport
  // itself (datagrams, handshakes, acks, keepalives, retransmissions) runs on
  // the hub's network thread, so a paused interface (a modal OS dialog, a
  // window the system stopped drawing, App Nap) never drops a link; with the
  // in-memory test fabric it runs inside poll, deterministically, on `nowSec`.
  void poll(double nowSec);
  bool threaded() const { return threaded_; }
  // Wall clock (seconds), replaceable for tests.
  std::function<int64_t()> wallSec;
  const VersionInfo& version() const { return version_; }

  // ---- invites (any member) ----
  enum class InviteStatus { Preparing, Ready, Connecting, Connected, Expired, Failed };
  struct InviteView {
    std::string id;
    std::string code;          // ready when status >= Ready
    InviteStatus status = InviteStatus::Preparing;
    int64_t expires = 0;
    std::string joinerName;    // after the handshake
    std::string safety;        // safety code to compare
    std::string error;
  };
  // `readmit`: an admin's re-invite of someone who was removed (only such an invite lets them back).
  std::string createInvite(int minutes, SiteId readmit = 0);
  const InviteView* invite(const std::string& id) const;
  std::vector<InviteView> invites() const;
  void cancelInvite(const std::string& id);
  // The invitee's reply code (its candidates) for one of our invites.
  bool acceptReply(const std::string& code, std::string* err = nullptr);

  // ---- joining (invitee; the session is a Session::join one) ----
  enum class JoinStatus { None, Connecting, NeedReply, Connected, Refused, Failed };
  struct JoinView {
    JoinStatus status = JoinStatus::None;
    std::string reply;     // our reply code (shown when the inviter does not see us quickly)
    std::string message;   // refusal / failure in words
    std::string safety;
    double since = 0;
  };
  bool join(const InviteInfo& info, std::string* err = nullptr);
  const JoinView& joinView() const { return join_; }

  // ---- reconnecting without a third collaborator ----
  std::string reconnectCode(SiteId peer);
  bool acceptReconnectCode(const std::string& code, std::string* err = nullptr);

  // ---- links ----
  struct LinkView {
    SiteId site = 0;
    // Waiting: they are offline (or unreachable at their last addresses); a link
    // listens for them and retries, a reconnect code helps when that fails.
    enum class State { None, Connecting, Direct, Relayed, Failed, Waiting } state = State::None;
    int attempts = 0;     // reconnect attempts that did not connect (Waiting)
    std::string path;     // host / srflx / prflx / relay / mapped
    std::string via;      // relayed through this collaborator's name
    double rttMs = -1;
    std::string error;
  };
  std::vector<LinkView> links() const;
  LinkView link(SiteId site) const;

  std::function<void(const std::string& text, bool error)> onNotice;
  struct Refusal {
    std::string who;              // the other person's name
    std::string theirVersion, myVersion;
    bool weAreOlder = false;      // we must update (else: they must)
    bool sameVersionOtherBuild = false;
  };
  std::function<void(const Refusal&)> onVersionRefusal;

  // Tests: a joiner learns the inviter's candidates only this many seconds
  // after joining (ICE that takes long to connect). From GRIDLINE_COLLAB_TEST_ICE_DELAY.
  double testIceDelaySec = 0;
  // statistics (tests)
  size_t linkCount() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return links_.size();
  }
  // One line per link record (kind, peer, ICE and handshake state, age, last heard): dev dumps.
  std::string describeLinks() const;

 private:
  struct Inbox {
    std::mutex mu;
    std::deque<std::string> q;
  };
  struct Wake {  // datagrams arrived: the network thread runs at once
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
  };
  struct LinkRec {
    int id = 0;
    enum class Kind { InviteHost, InviteJoin, Member } kind = Kind::Member;
    bool initiator = false;
    SiteId peer = 0;
    std::string inviteId;
    std::unique_ptr<Endpoint> ep;
    std::unique_ptr<SecureLink> sl;
    std::shared_ptr<Inbox> inbox;
    double created = 0, startedAt = -1, iceAt = -1;
    bool started = false, up = false, dead = false, sigSent = false;
    // network thread -> UI thread (poll), under mu_
    std::vector<std::string> uiIn;                // a responder's handshake packets (acceptance admits members)
    std::vector<std::pair<int, std::string>> rx;  // messages received
    bool openedEv = false, closedEv = false;
    std::string closeWhy;
    std::string remoteDesc;
    std::string error;
    std::string heldDesc;   // tests: the remote description, applied at heldUntil
    double heldUntil = -1;
  };
  struct InviteRec {
    InviteView view;
    InviteInfo info;
    int link = 0;
    double gatheredAt = -1;
    bool used = false;
    SiteId readmit = 0;
  };
  struct PeerNet {
    std::string lastDesc;        // their last ICE description we know
    uint16_t localPort = 0;      // the UDP port we used with them
    double nextTry = 0;
    double backoff = 10;
    int failures = 0;
    std::set<SiteId> theirLinks; // whom they reach directly (net.peers)
    double peersAt = -1;
    bool relayedUp = false;
    bool notified = false;       // a removed member was told (they refused the link knowing why)
    std::string name;
  };

  static const char* kindName(int kind);
  double clockSec() const;
  void startThread();
  void stopThread();
  void transportStep(double t);
  LinkRec* newLink(LinkRec::Kind kind, bool initiator, const EndpointConfig& ec, SecureLink::Hs hs);
  void dropLink(int id, const std::string& why);
  LinkRec* directLink(SiteId peer);
  SecureLink::Hs memberHs(SiteId peer, bool initiator, const Member& m);
  crypto::Key memberPsk(const Member& m) const;
  EndpointConfig memberEndpoint(SiteId peer, const crypto::Key& psk, bool initiator);
  void startMemberLink(SiteId peer, bool initiator, const std::string& remoteDesc);
  Json helloJson() const;
  bool checkVersion(const Json& theirHello, const std::string& who, Json* refusal);
  bool shunned(SiteId peer) const;  // removed or departed: no links, relay or introductions
  static bool isNotice(const std::string& wire);
  void onLinkOpen(LinkRec& l);
  void onLinkMessage(LinkRec& l, int stream, const std::string& data);
  void handleNet(SiteId from, const Message& m);
  void sendTo(SiteId to, int stream, const std::string& wire);
  void sendNet(SiteId to, const Message& m);
  void relayRoutes();
  void autoReconnect();
  void loadState();
  void saveState();
  double now() const { return now_; }
  uint64_t nowMs() const { return static_cast<uint64_t>(now_ * 1000.0); }

  std::unique_ptr<Fabric> fabric_;
  crypto::Identity id_;
  Profile profile_;
  VersionInfo version_;
  std::string stateDir_;
  Session* session_ = nullptr;
  std::map<int, std::unique_ptr<LinkRec>> links_;
  int nextLink_ = 1;
  std::map<std::string, InviteRec> invites_;
  JoinView join_;
  InviteInfo joinInfo_;
  int joinLink_ = 0;
  std::map<SiteId, PeerNet> net_;
  std::map<SiteId, SiteId> via_;  // relayed peer -> collaborator in between
  std::map<SiteId, double> shunnedSince_;  // removed / departed: their links close after a short grace (the notice goes out)
  double now_ = 0, lastPeers_ = -1, lastSave_ = -1, lastPoll_ = -1;
  bool stateLoaded_ = false;
  // network thread (real networks); mu_ guards everything above
  mutable std::recursive_mutex mu_;
  std::shared_ptr<Wake> wake_ = std::make_shared<Wake>();
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool threaded_ = false;
  std::chrono::steady_clock::time_point born_ = std::chrono::steady_clock::now();
};

// "Sam is on Gridline 0.7.1; update to collaborate." / "Alex needs to update Gridline (to 0.7.1) before joining."
std::string fmtVersionOlder(const Hub::Refusal& r);
std::string fmtVersionNewer(const Hub::Refusal& r);

}  // namespace gl::collab

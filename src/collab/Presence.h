// Gridline - J-146 collaboration: who is working on a collaborative project right now,
// learnt without a server and without opening the project (the welcome screen's
// Collaborative view).
//
// A project's card (<collab>/projects/<id>.card.json) is a summary the Controller writes
// while the project is open: members (keys, names, states), my role, pending changes, the
// Sync review, the version record. A presence probe is a member-link handshake (Hub.h:
// the same derived ICE credentials, roles and keys) carrying "probe": the other side's Hub,
// when it has the project open, refuses the link on purpose with a presence answer (since
// when it is open, how long its user has been idle, who is with them). Nothing is opened
// or synced; only members can complete the handshake or read the answer. No answer is
// never taken for "offline": a network that blocks direct connections looks the same.
// Design notes: docs/collaboration.md ("Presence on the welcome screen").
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../core/Json.h"
#include "Crdt.h"
#include "Crypto.h"
#include "Hub.h"
#include "Link.h"
#include "Profile.h"

namespace gl::collab {

class Session;
class SecureLink;

// ---------------------------------------------------------------- card
struct CardMember {
  SiteId site = 0;
  crypto::Key signPublic{}, dhPublic{};
  std::string name;
  uint32_t color = 0;
  std::string avatarHash;
  enum class State { Active, Removed, Left } state = State::Active;
  uint64_t lastSyncMs = 0;  // wall ms of the last exchange with them (this computer)
  std::string fingerprint() const { return crypto::fingerprintOf(signPublic); }
};

struct ProjectCard {
  std::string id, name, folder;
  SiteId me = 0, admin = 0;
  std::vector<CardMember> members;  // everyone ever admitted (me included)
  size_t pending = 0;               // my changes no collaborator has confirmed yet
  size_t review = 0;                // open Sync review items (conflicting offline edits)
  std::string minAppVersion;
  uint64_t lastContactMs = 0;       // last exchange with any collaborator
  uint64_t savedAt = 0;             // wall ms the card was written
  bool valid() const { return !id.empty() && me != 0; }
  const CardMember* member(SiteId s) const;
};

// The card of the session's project (members, states, counters) at `nowMs`.
ProjectCard cardOf(const Session& s, uint64_t nowMs);
std::string cardPath(const std::string& collabDir, const std::string& projectId);
bool writeCard(const std::string& collabDir, const ProjectCard& c);
bool readCard(const std::string& collabDir, const std::string& projectId, ProjectCard& out);
Json cardToJson(const ProjectCard& c);
bool cardFromJson(const Json& j, ProjectCard& out);

// ---------------------------------------------------------------- probe results
enum class PresenceState {
  Unknown,       // never asked (no address known, presence off, update needed)
  Checking,      // a probe is on its way, nothing known yet
  Working,       // they have the project open right now (answered)
  OtherVersion,  // they have it open, on another Gridline (`app`)
  Online,        // Gridline is running on their computer (the welcome screen), the project is not open
  NoAnswer,      // not reachable right now: offline, or a network that blocks a direct check
  Hidden,        // they chose not to show that they are online
  RemovedMe,     // their answer: an admin removed me from the project (signed notice)
  Ended,         // their answer: the admin stopped sharing it
};
const char* presenceStateName(PresenceState s);

struct PresenceResult {
  PresenceState state = PresenceState::Unknown;
  int64_t checkedAt = 0;   // wall s of the last answer (or of giving up)
  int64_t since = 0;       // Working: wall s they opened the project
  int64_t idle = -1;       // Working: s since their last input when they answered (-1 unknown)
  std::string app;         // their Gridline version (when they answered)
  std::vector<SiteId> with;  // Working: members linked with them right now
  std::string viaName;     // Working through someone else's answer: who told us
  SiteId via = 0;
  Profile profile;         // as they sent it (name, colour, avatar hash, email if shared)
  std::string byName;      // RemovedMe / Ended: who did it
  int failures = 0;        // consecutive probes without an answer
};

// When the next probe of one member is due after a result (seconds).
double nextProbeDelay(PresenceState s, int failures);
inline constexpr double kProbeTimeoutSec = 20;
// A responder probe (their Hub starts the handshake) shows "no answer" after kProbeTimeoutSec
// but listens this long: a Hub tells a removed member the news only once a minute.
inline constexpr double kProbeListenSec = 75;
inline constexpr double kPresenceFreshSec = 45;   // an answer counts as "now" this long
inline constexpr double kPresenceIdleStopSec = 2;  // probes stop this long after the view hides
inline constexpr int kMaxProbes = 4;

// Last time each member was seen working on a project (probes and sessions), per project:
// <collab>/projects/<id>.seen.json, site -> wall s.
std::map<SiteId, int64_t> readSeen(const std::string& collabDir, const std::string& projectId);
bool writeSeen(const std::string& collabDir, const std::string& projectId, const std::map<SiteId, int64_t>& seen);

// ---------------------------------------------------------------- prober
struct ProbeTarget {
  std::string projectId;
  SiteId site = 0;
  crypto::Key dhPublic{};
  std::string lastDesc;    // their last ICE description (Hub state)
  uint16_t localPort = 0;  // the UDP port this computer used with them
  std::string name;
};

class PresenceProber {
 public:
  PresenceProber(std::unique_ptr<Fabric> fabric, const crypto::Identity& id, Profile profile, VersionInfo version);
  ~PresenceProber();
  PresenceProber(const PresenceProber&) = delete;
  PresenceProber& operator=(const PresenceProber&) = delete;

  // What to watch (replaces the list; probes of targets no longer listed stop).
  void setTargets(std::vector<ProbeTarget> targets);
  // "Show that I am online": off, nothing is probed and nothing answered.
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }
  void setProfile(const Profile& p) { profile_ = p; }
  // The view is visible: probes run; kPresenceIdleStopSec after the last call they stop.
  void want(double now);
  // Per frame (UI thread): datagrams, handshakes, timeouts, the schedule.
  void poll(double now);
  // A project opened here: its probes stop at once (its Hub uses those ports).
  void forget(const std::string& projectId);
  // The result for one member (nullptr: nothing known yet).
  const PresenceResult* result(const std::string& projectId, SiteId site) const;
  const std::map<SiteId, PresenceResult>* results(const std::string& projectId) const;
  // Something changed since the last call (the view redraws, the seen cache is written).
  bool takeChanged();
  // Wall clock (seconds), replaceable for tests.
  std::function<int64_t()> wallSec;
  // Statistics (tests): probes started, open endpoints.
  int started() const { return started_; }
  size_t inFlight() const { return probes_.size(); }
  bool running() const { return running_; }
  // Tests / dev: retry everything now (the backoff restarts).
  void refreshNow();

 private:
  struct Probe;
  void startProbe(const ProbeTarget& t, double now);
  void finish(Probe& p, PresenceState st, const Json* answer, double now);
  void applyAnswer(Probe& p, const Json& answer, double now);
  Json helloJson(const std::string& projectId) const;
  std::string key(const std::string& pid, SiteId s) const;

  std::unique_ptr<Fabric> fabric_;
  crypto::Identity id_;
  Profile profile_;
  std::string app_, build_;
  int protocol_ = 0;
  bool enabled_ = true, running_ = false;
  double wantUntil_ = -1, now_ = 0;
  std::vector<ProbeTarget> targets_;
  std::map<std::string, double> nextAt_;  // key -> when the next probe of that member may start
  std::map<std::string, std::map<SiteId, PresenceResult>> results_;
  std::vector<std::unique_ptr<Probe>> probes_;
  int started_ = 0;
  bool changed_ = false;
  double lastRefresh_ = -1000;
};

}  // namespace gl::collab

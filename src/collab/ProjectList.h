// Gridline - J-146 collaboration: the welcome screen's list of collaborative projects, as
// data (no UI): every project this computer knows, with its members, my role, what needs
// attention and who is working on it right now. Built from the known-projects list
// (Controller), the cards (Presence.h), the presence answers, the "last seen" cache and,
// for the project open here, the live session. Pure: unit tested.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Presence.h"

namespace gl::collab {

struct ListProject {  // one entry of known-projects.json
  std::string id, name, folder;
  uint64_t openedMs = 0;
  enum class State { Active, Removed, Ended, Missing } state = State::Active;
  std::string byName, reason;
  uint64_t at = 0;
};

// The project open here: its live session decides (no probes).
struct LiveProject {
  std::string id;
  std::map<SiteId, bool> connected;  // member -> linked right now (direct or relayed)
  std::map<SiteId, Profile> profiles;  // what they told this session
  bool admin = false;
  size_t pending = 0, review = 0;
};

struct ListContext {
  SiteId me = 0;
  std::string myName;
  std::string myEmail;
  std::string myVersion;            // this build ("0.7.1")
  std::string gateLatest;           // the version gate wants this release ("" = no update required)
  bool presenceOn = true;           // "Show that I am online to collaborators"
  int64_t nowSec = 0;
  const LiveProject* live = nullptr;
};

struct ListMember {
  SiteId site = 0;
  std::string name, fingerprint, avatarHash;
  uint32_t color = 0;
  bool me = false;
  bool mineElsewhere = false;  // probably this user on another computer (same email, else same name)
  bool admin = false;
  PresenceState presence = PresenceState::Unknown;
  bool now = false;            // working on it right now (fresh answer, or live here)
  bool live = false;           // from the session open here
  int64_t since = 0, idle = -1;
  int64_t checkedAt = 0;       // when the presence result came
  int64_t seenAt = 0;          // last seen working on it (cache)
  uint64_t lastSyncMs = 0;
  std::string viaName;         // known through this member's answer
  std::string app;             // their version (OtherVersion)
};

struct ListRow {
  std::string id, name, folder, projectFile;
  ListProject::State state = ListProject::State::Active;
  bool hasCard = false;
  bool admin = false;          // my role
  bool openHere = false;
  bool removedNotice = false;  // a collaborator told this computer it was removed (signed notice), not yet seen by opening it
  std::string byName, reason;  // removed / ended: by whom, why
  uint64_t at = 0;
  uint64_t openedMs = 0;
  std::vector<ListMember> members;  // active members: me first, then the others by name
  std::vector<std::string> workingNow;  // others working on it right now (names)
  int64_t workingSince = 0;    // the earliest "since" of those
  size_t pending = 0, review = 0;
  std::string needVersion;     // this project or the gate needs a newer Gridline
  uint64_t lastSyncMs = 0;     // newest exchange with anyone
  int64_t lastSeenAt = 0;      // newest "seen working on it" of the others
  bool unreachable = false;    // every other member: no answer (unknown, never "offline")
  bool probing = false;        // presence checks run for it
};

// Rows in the known list's order (most recently opened first; stable while presence changes).
std::vector<ListRow> buildProjectList(const std::vector<ListProject>& known, const std::map<std::string, ProjectCard>& cards,
                                      const std::map<std::string, std::map<SiteId, PresenceResult>>& results,
                                      const std::map<std::string, std::map<SiteId, int64_t>>& seen, const ListContext& ctx);

// Whom to probe for a row's project (active members other than me, with the addresses the
// Hub kept: <collab>/projects/<id>.json). Nothing for a project that is open here, not
// active, needs a newer Gridline, or when presence is off.
std::vector<ProbeTarget> probeTargets(const std::string& collabDir, const ListRow& row, const ProjectCard& card, const ListContext& ctx);

// "Sam", "Sam and Kim", "Sam, Kim and 2 others" (the words come from `and` / `others`).
std::string joinNames(const std::vector<std::string>& names, const std::string& andWord, const std::string& othersFmt);

}  // namespace gl::collab

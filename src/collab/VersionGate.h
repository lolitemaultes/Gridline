// Gridline - J-111 collaboration: the version gate. Collaborating needs the
// newest Gridline: the running version must be at least the newest eligible
// release (not a draft, not a prerelease, with a download for this computer;
// beta users compare with the newest stable one; builds newer than it pass).
// Built on the J-89 update check (core/Update.h): one request to the GitHub
// releases list, cached for an hour (the setting keeps the last known latest
// version), never per frame. Skip-version / remind-later / automatic checks
// off only govern the update popup, not this gate. When the check fails
// (offline, rate limited) the last known latest version decides; with none,
// collaboration is allowed and the peer handshake (Hub) is authoritative.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/Update.h"

namespace gl {
class Settings;
}

namespace gl::collab {

inline constexpr int64_t kGateCacheSec = 3600;

struct GateState {
  enum class Kind { Unknown, UpToDate, UpdateRequired };
  Kind kind = Kind::Unknown;
  std::string latest;        // newest eligible release ("" = unknown)
  std::string current;
  int64_t checkedAt = 0;     // when `latest` was learnt
  bool couldNotCheck = false;  // the last check failed (offline, rate limited, server)
  std::string detail;        // why (English, logs)
  bool allows() const { return kind != Kind::UpdateRequired; }
};

// The newest release that counts for collaboration, or nullptr.
const update::Release* newestEligible(const std::vector<update::Release>& releases, const update::Target& t);
// Pure decision from a release list (tests).
GateState evaluate(const std::vector<update::Release>& releases, const std::string& current, const update::Target& t);
// Decision from a cached latest version.
GateState fromCache(const std::string& latest, const std::string& current, int64_t checkedAt);

// The cached state in the settings plus one check through `fetch`.
class VersionGate {
 public:
  explicit VersionGate(Settings& settings, std::string current);
  GateState state(int64_t now) const;     // from the cache (no request)
  bool due(int64_t now) const;            // the cache is older than an hour
  // One request (call off the UI thread in the app). Persists the result.
  GateState check(const update::Fetcher& fetch, int64_t now, const update::Target& t = update::currentTarget());
  // A result fetched elsewhere (the app's worker thread) is stored.
  void store(const GateState& s);

 private:
  Settings& settings_;
  std::string current_;
};

// How Gridline was installed (the update screen offers the store too).
enum class InstallKind { Official, Flatpak, DistroPackage };
InstallKind installKind();

}  // namespace gl::collab

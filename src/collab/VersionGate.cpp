// Gridline - J-111 collaboration: version gate. See VersionGate.h.
#include "VersionGate.h"
#include <cstdlib>
#include <filesystem>
#include "../core/Platform.h"
#include "../core/Settings.h"

namespace gl::collab {

const update::Release* newestEligible(const std::vector<update::Release>& releases, const update::Target& t) {
  const update::Release* best = nullptr;
  for (const auto& r : releases) {
    if (r.draft || r.prerelease || !r.version.valid || !r.version.pre.empty()) continue;
    // a tag without the installers yet (the release workflow still runs) does not lock anyone out
    if (!update::pickAsset(r, t)) continue;
    if (!best || update::compare(r.version, best->version) > 0) best = &r;
  }
  return best;
}

GateState fromCache(const std::string& latest, const std::string& current, int64_t checkedAt) {
  GateState s;
  s.current = current;
  s.latest = latest;
  s.checkedAt = checkedAt;
  const update::Version l = update::parseVersion(latest), c = update::parseVersion(current);
  if (!l.valid || !c.valid) {
    s.kind = GateState::Kind::Unknown;
    return s;
  }
  s.kind = update::compare(c, l) >= 0 ? GateState::Kind::UpToDate : GateState::Kind::UpdateRequired;
  return s;
}

GateState evaluate(const std::vector<update::Release>& releases, const std::string& current, const update::Target& t) {
  const update::Release* r = newestEligible(releases, t);
  if (!r) {
    GateState s;
    s.current = current;
    s.kind = GateState::Kind::Unknown;
    s.detail = "no eligible release";
    return s;
  }
  return fromCache(update::toString(r->version), current, 0);
}

VersionGate::VersionGate(Settings& settings, std::string current) : settings_(settings), current_(std::move(current)) {}

GateState VersionGate::state(int64_t now) const {
  (void)now;
  return fromCache(settings_.getString("collab.latestVersion"), current_, static_cast<int64_t>(settings_.getNumber("collab.latestCheckedAt")));
}

bool VersionGate::due(int64_t now) const {
  const int64_t at = static_cast<int64_t>(settings_.getNumber("collab.latestCheckedAt"));
  return at <= 0 || now < at || now - at >= kGateCacheSec;
}

void VersionGate::store(const GateState& s) {
  if (s.couldNotCheck || s.latest.empty()) return;
  settings_.set("collab.latestVersion", s.latest);
  settings_.set("collab.latestCheckedAt", static_cast<double>(s.checkedAt));
}

GateState VersionGate::check(const update::Fetcher& fetch, int64_t now, const update::Target& t) {
  const update::HttpResult r = fetch(update::kListUrl);
  GateState cached = state(now);
  auto failed = [&](const std::string& why) {
    cached.couldNotCheck = true;
    cached.detail = why;
    return cached;  // the last known latest version still decides; none: allowed (the handshake decides)
  };
  if (r.offline || r.status == 0) return failed("offline");
  if (r.status == 403 || r.status == 429) return failed("rate limited");
  if (r.status != 200) return failed("HTTP " + std::to_string(r.status));
  std::vector<update::Release> rel;
  std::string err;
  if (!update::parseReleases(r.body, rel, &err)) return failed(err.empty() ? "bad response" : err);
  GateState s = evaluate(rel, current_, t);
  if (s.latest.empty()) {
    s.checkedAt = now;
    return s;  // no installable release at all: nothing to require
  }
  s.checkedAt = now;
  store(s);
  return s;
}

InstallKind installKind() {
  if (std::getenv("FLATPAK_ID")) return InstallKind::Flatpak;
  std::error_code ec;
  if (std::filesystem::exists("/.flatpak-info", ec)) return InstallKind::Flatpak;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  if (std::getenv("APPIMAGE")) return InstallKind::Official;
  const std::string exe = platform::executablePath();
  if (exe.rfind("/usr/bin/", 0) == 0 || exe.rfind("/usr/lib", 0) == 0 || exe.rfind("/usr/local/bin/", 0) == 0 ||
      exe.rfind("/usr/pkg/", 0) == 0 || exe.rfind("/usr/games/", 0) == 0)
    return InstallKind::DistroPackage;
#endif
  return InstallKind::Official;
}

}  // namespace gl::collab

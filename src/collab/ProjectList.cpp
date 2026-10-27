// Gridline - J-146 collaboration: the welcome screen's project list. See ProjectList.h.
#include "ProjectList.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include "Log.h"
#include "Session.h"

namespace fs = std::filesystem;

namespace gl::collab {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool newerThan(const std::string& want, const std::string& have) { return !want.empty() && compareVersions(have, want) < 0; }

}  // namespace

std::string joinNames(const std::vector<std::string>& names, const std::string& andWord, const std::string& othersFmt) {
  if (names.empty()) return {};
  if (names.size() == 1) return names[0];
  if (names.size() == 2) return names[0] + " " + andWord + " " + names[1];
  if (names.size() == 3) return names[0] + ", " + names[1] + " " + andWord + " " + names[2];
  char b[96];
  std::snprintf(b, sizeof b, othersFmt.c_str(), static_cast<int>(names.size() - 2));
  return names[0] + ", " + names[1] + " " + andWord + " " + b;
}

std::vector<ListRow> buildProjectList(const std::vector<ListProject>& known, const std::map<std::string, ProjectCard>& cards,
                                      const std::map<std::string, std::map<SiteId, PresenceResult>>& results,
                                      const std::map<std::string, std::map<SiteId, int64_t>>& seen, const ListContext& ctx) {
  std::vector<ListRow> out;
  const std::string myName = lower(cleanName(ctx.myName)), myEmail = lower(ctx.myEmail);
  for (const ListProject& k : known) {
    ListRow r;
    r.id = k.id;
    r.name = k.name;
    r.folder = k.folder;
    r.state = k.state;
    r.byName = k.byName;
    r.reason = k.reason;
    r.at = k.at;
    r.openedMs = k.openedMs;
    if (k.state != ListProject::State::Missing) r.projectFile = Session::projectFileOf(k.folder);
    const bool openHere = ctx.live && !ctx.live->id.empty() && ctx.live->id == k.id;
    r.openHere = openHere;
    auto cit = cards.find(k.id);
    const ProjectCard* card = cit != cards.end() ? &cit->second : nullptr;
    r.hasCard = card != nullptr;
    const auto* res = [&]() -> const std::map<SiteId, PresenceResult>* {
      auto it = results.find(k.id);
      return it == results.end() ? nullptr : &it->second;
    }();
    const auto* seenMap = [&]() -> const std::map<SiteId, int64_t>* {
      auto it = seen.find(k.id);
      return it == seen.end() ? nullptr : &it->second;
    }();
    if (card) {
      r.admin = card->admin == card->me;
      r.pending = card->pending;
      r.review = card->review;
      if (newerThan(card->minAppVersion, ctx.myVersion)) r.needVersion = card->minAppVersion;
      r.lastSyncMs = card->lastContactMs;
      for (const CardMember& cm : card->members) {
        if (cm.state != CardMember::State::Active) continue;  // removed members and people who left are not shown
        ListMember m;
        m.site = cm.site;
        m.name = cm.name;
        m.fingerprint = cm.fingerprint();
        m.avatarHash = cm.avatarHash;
        m.color = cm.color;
        m.me = cm.site == card->me;
        m.admin = cm.site == card->admin;
        m.lastSyncMs = cm.lastSyncMs;
        r.lastSyncMs = std::max<uint64_t>(r.lastSyncMs, cm.lastSyncMs);
        if (seenMap)
          if (auto s = seenMap->find(cm.site); s != seenMap->end()) m.seenAt = s->second;
        if (!m.me && openHere) {
          auto c = ctx.live->connected.find(cm.site);
          m.live = true;
          m.now = c != ctx.live->connected.end() && c->second;
          m.presence = m.now ? PresenceState::Working : PresenceState::NoAnswer;
          m.checkedAt = ctx.nowSec;
          if (m.now) m.seenAt = ctx.nowSec;
          if (auto p = ctx.live->profiles.find(cm.site); p != ctx.live->profiles.end()) {
            if (!p->second.name.empty()) m.name = p->second.name;
            if (p->second.color) m.color = p->second.color;
            if (!p->second.avatarHash().empty()) m.avatarHash = p->second.avatarHash();
            if (!myEmail.empty() && lower(p->second.email) == myEmail) m.mineElsewhere = true;
          }
        } else if (!m.me && res) {
          if (auto p = res->find(cm.site); p != res->end()) {
            const PresenceResult& pr = p->second;
            m.presence = pr.state;
            m.checkedAt = pr.checkedAt;
            m.since = pr.since;
            m.idle = pr.idle;
            m.viaName = pr.viaName;
            m.app = pr.app;
            const bool fresh = ctx.nowSec - pr.checkedAt <= static_cast<int64_t>(kPresenceFreshSec);
            m.now = pr.state == PresenceState::Working && fresh;
            if (pr.state == PresenceState::Working) m.seenAt = std::max(m.seenAt, pr.checkedAt);
            if (!pr.profile.name.empty()) m.name = pr.profile.name;
            if (pr.profile.color) m.color = pr.profile.color;
            if (!pr.profile.avatarHash().empty()) m.avatarHash = pr.profile.avatarHash();
            if (!myEmail.empty() && lower(pr.profile.email) == myEmail) m.mineElsewhere = true;
            if (pr.state == PresenceState::RemovedMe) r.removedNotice = true, r.byName = pr.byName.empty() ? m.name : pr.byName;
          }
        }
        if (!m.me && !myName.empty() && lower(cleanName(m.name)) == myName) m.mineElsewhere = true;
        r.members.push_back(m);
      }
      std::stable_sort(r.members.begin(), r.members.end(), [](const ListMember& a, const ListMember& b) {
        if (a.me != b.me) return a.me;
        if (a.now != b.now) return a.now;
        return lower(a.name) < lower(b.name);
      });
      int others = 0, silent = 0;
      for (const ListMember& m : r.members) {
        if (m.me) continue;
        ++others;
        if (m.now) {
          r.workingNow.push_back(m.mineElsewhere ? std::string() : m.name);
          if (m.since > 0) r.workingSince = r.workingSince ? std::min(r.workingSince, m.since) : m.since;
        }
        if (m.presence == PresenceState::NoAnswer) ++silent;
        r.lastSeenAt = std::max(r.lastSeenAt, m.seenAt);
      }
      r.unreachable = others > 0 && silent == others;
    }
    if (openHere) {
      r.admin = ctx.live->admin;
      r.pending = ctx.live->pending;
      r.review = ctx.live->review;
    }
    if (r.needVersion.empty() && !ctx.gateLatest.empty()) r.needVersion = ctx.gateLatest;
    r.probing = card && !openHere && ctx.presenceOn && r.state == ListProject::State::Active && r.needVersion.empty() && !r.removedNotice;
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<ProbeTarget> probeTargets(const std::string& collabDir, const ListRow& row, const ProjectCard& card, const ListContext& ctx) {
  std::vector<ProbeTarget> out;
  if (!row.probing || !ctx.presenceOn || row.openHere || card.id != row.id) return out;
  // their addresses and our ports, as the Hub kept them (written while the project was open)
  std::map<SiteId, std::pair<std::string, uint16_t>> net;
  {
    std::string t;
    Json j;
    if (readFileText((fs::path(collabDir) / "projects" / (row.id + ".json")).string(), t, 1 << 20) && jsonParse(t, j))
      if (const Json* peers = j.find("peers"); peers && peers->is(Json::Type::Object))
        for (const auto& [k, v] : peers->object)
          net[siteFromHex(k)] = {v.text("desc"), static_cast<uint16_t>(std::clamp(v.num("port", 0), 0.0, 65535.0))};
  }
  for (const CardMember& m : card.members) {
    if (m.site == card.me || m.state != CardMember::State::Active) continue;
    ProbeTarget t;
    t.projectId = row.id;
    t.site = m.site;
    t.dhPublic = m.dhPublic;
    t.name = m.name;
    if (auto it = net.find(m.site); it != net.end()) {
      t.lastDesc = it->second.first;
      t.localPort = it->second.second;
    }
    out.push_back(t);
  }
  return out;
}

}  // namespace gl::collab

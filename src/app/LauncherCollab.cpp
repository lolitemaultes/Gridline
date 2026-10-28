// Gridline - J-146: the welcome screen's Collaborative view (see LauncherCollab.h).
#include "LauncherCollab.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"
#include "../collab/Controller.h"
#include "../collab/ProjectList.h"
#include "../collab/VersionGate.h"
#include "../core/I18n.h"
#include "../core/Recents.h"
#include "App.h"
#include "CollabUi.h"
#include "DevSignal.h"
#include "LauncherUi.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "UpdateUi.h"
#include "Widgets.h"

namespace gl::launchercollab {

namespace {

namespace fs = std::filesystem;
using i18n::tr;
using namespace gl::collab;
using St = ListProject::State;

enum class Kind { Join, New, Share, Section, Project, Empty, Settings };
struct Entry {
  Kind kind = Kind::Project;
  int row = -1;       // Project: index in Model::rows
  std::string label;  // Section
};

struct Model {
  std::vector<ListProject> known;
  std::map<std::string, ProjectCard> cards;
  std::map<std::string, std::map<SiteId, int64_t>> seen;
  std::map<std::string, std::string> pictures;  // fingerprint -> JPEG (collaborators' pictures kept here)
  std::string myPicture;
  double diskAt = -100;
  std::vector<ListRow> rows;
  std::vector<Entry> entries;
  float settingsH = 760;  // measured each frame (the next layout uses it)
  std::map<ImGuiID, float> hover;
  ImGuiID tipId = 0;  // the tile under the pointer, since when (its tooltip waits a moment)
  double tipSince = 0;
  ImVec2 tipSize = ImVec2(300, 200);  // the tooltip's size last frame (placing it inside the window)
  ListRow menu;           // the tile whose menu is open
  bool openMenu = false;
  bool menuShown = false;          // the menu showed this frame (dev report)
  std::map<std::string, ImVec4> tileRects;  // project id -> last drawn tile (x0, y0, x1, y1; dev report)
  bool presenceOn = true, probing = false, checking = false, hasTargets = false;
  int live = 0;
  std::string openName;  // the project open here when it is not collaborative ("" = none / collaborative)
};
Model& M() {
  static Model m;
  return m;
}

int64_t wallNow() { return static_cast<int64_t>(std::time(nullptr)); }

// Hover easing per item (snaps with Interface animations off), as the other views.
float animate(ImGuiID id, bool on) {
  float& v = M().hover[id];
  const float target = on ? 1.0f : 0.0f;
  if (!scrollui::animations()) return v = target;
  const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  v += (target - v) * k;
  if (std::fabs(v - target) < 0.01f) v = target;
  return v;
}

ImVec2 measure(ImFont* f, float size, const char* s) { return f ? f->CalcTextSizeA(size, FLT_MAX, 0, s) : ImVec2(0, size); }

void textAt(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* s, float maxW) {
  theme::textEllipsis(dl, f, size, ImVec2(std::round(p.x), std::round(p.y)), maxW, col, s);
}

std::string fmtS(const char* f, const std::string& a) {
  char b[512];
  std::snprintf(b, sizeof b, f, a.c_str());
  return b;
}
std::string fmtN(const char* f, long long n) {
  char b[256];
  std::snprintf(b, sizeof b, f, n);
  return b;
}

// "just now", "5 min ago", "3 h ago", "yesterday", "4 days ago", "12 Oct": inside a sentence.
std::string ago(int64_t then, int64_t now) {
  const recents::Age g = recents::age(then, now);
  switch (g.unit) {
    case recents::AgeUnit::None: return "";
    case recents::AgeUnit::JustNow: return tr("just now");
    case recents::AgeUnit::Minutes: return fmtN(tr("%lld min ago"), g.count);
    case recents::AgeUnit::Hours: return fmtN(tr("%lld h ago"), g.count);
    case recents::AgeUnit::Yesterday: return tr("yesterday");
    case recents::AgeUnit::Days: return fmtN(tr("%lld days ago"), g.count);
    case recents::AgeUnit::Date: {
      const std::time_t t = static_cast<std::time_t>(then);
      std::tm tmv{};
#ifdef _WIN32
      localtime_s(&tmv, &t);
#else
      localtime_r(&t, &tmv);
#endif
      char b[32];
      std::strftime(b, sizeof b, "%d %b", &tmv);
      return b[0] == '0' ? std::string(b + 1) : std::string(b);
    }
  }
  return "";
}

std::string clockText(int64_t t) {
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &tt);
#else
  localtime_r(&tt, &tmv);
#endif
  char b[16];
  std::strftime(b, sizeof b, "%H:%M", &tmv);
  return b;
}

const char* revealLabel() {
#if defined(__APPLE__)
  return i18n::N_("Show in Finder");
#elif defined(_WIN32)
  return i18n::N_("Show in Explorer");
#else
  return i18n::N_("Show in Files");
#endif
}

bool matches(const ListRow& r, const char* q) {
  if (!q || !*q) return true;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string n = lower(q);
  if (lower(r.name).find(n) != std::string::npos) return true;
  for (const auto& m : r.members)
    if (lower(m.name).find(n) != std::string::npos) return true;
  return false;
}

uint32_t colorFor(const ListMember& m) {
  if (m.color) return m.color;
  static const uint32_t kColors[] = {0x4FB8B0, 0xD8775A, 0x8E7CE0, 0x6CB86A, 0xD9B44A, 0x5A9BD8, 0xC86BA8};
  uint32_t h = 2166136261u;
  for (char c : m.fingerprint) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
  return kColors[h % 7];
}

// ------------------------------------------------------------------ the model
LiveProject liveOf(Session& s) {
  LiveProject l;
  l.id = s.projectId();
  for (const auto& [site, pv] : s.peers()) {
    l.connected[site] = pv.connected;
    l.profiles[site] = pv.profile;
  }
  l.admin = s.isAdmin();
  l.pending = s.unackedBatches();
  l.review = s.isAdmin() ? s.conflicts().openCount() : 0;
  return l;
}

void refresh(App& a, const char* search) {
  Model& m = M();
  Controller* c = collabui::controller(a);
  m.rows.clear();
  m.entries.clear();
  m.live = 0;
  m.probing = false;
  m.checking = false;
  m.presenceOn = a.settings.getBool("collab.showOnline");
  Session* s = c ? c->session() : nullptr;
  m.openName.clear();
  if (a.home.projectOpen() && !(s && s->ready())) {
    const std::string pf = a.projectFile();
    m.openName = pf.empty() ? (a.doc.song().info.title.empty() ? std::string(tr("Untitled project")) : a.doc.song().info.title) : projectStemOf(pf);
  }
  if (c) {
    const bool fromDisk = a.now - m.diskAt > 2.0 || a.now < m.diskAt;
    if (fromDisk) {
      m.diskAt = a.now;
      m.known = c->listProjects();
      m.cards = c->cards();
      m.seen = c->seen();
      m.myPicture = c->profiles().profile().avatar;
      m.pictures.clear();
      for (const auto& [id, card] : m.cards)
        for (const auto& cm : card.members) {
          if (cm.avatarHash.empty() || m.pictures.count(cm.fingerprint())) continue;
          if (const Profile* p = c->profiles().known(cm.fingerprint()); p && !p->avatar.empty() && p->avatarHash() == cm.avatarHash)
            m.pictures[cm.fingerprint()] = p->avatar;
        }
    }
    ListContext ctx;
    ctx.me = c->profiles().identity().site();
    ctx.myName = c->profiles().profile().name;
    ctx.myEmail = c->profiles().profile().email;
    ctx.myVersion = c->version.app;
    static std::string gateLatest;
    static double gateAt = -100;
    if (fromDisk || a.now - gateAt > 2.0) {
      gateAt = a.now;
      const GateState& g = updui::gate(a);
      gateLatest = g.kind == GateState::Kind::UpdateRequired ? g.latest : std::string();
    }
    ctx.gateLatest = gateLatest;
    ctx.presenceOn = m.presenceOn;
    ctx.nowSec = wallNow();
    LiveProject live;
    if (s && s->ready() && !s->stopped() && a.home.projectOpen()) {
      live = liveOf(*s);
      ctx.live = &live;
    }
    std::map<std::string, std::map<SiteId, PresenceResult>> results;
    if (c->hasPresence())
      for (const auto& k : m.known)
        if (const auto* r = c->presence().results(k.id)) results[k.id] = *r;
    m.rows = buildProjectList(m.known, m.cards, results, m.seen, ctx);
    // whom to ask (only while this view shows: want() below)
    for (const ListRow& r : m.rows) m.probing = m.probing || r.probing;
    if (fromDisk) {  // (the addresses come from files: read with the rest)
      std::vector<ProbeTarget> targets;
      for (const ListRow& r : m.rows) {
        const auto it = m.cards.find(r.id);
        if (!r.probing || it == m.cards.end()) continue;
        for (ProbeTarget& t : probeTargets(c->collabDir(), r, it->second, ctx)) targets.push_back(std::move(t));
      }
      m.hasTargets = !targets.empty();
      if (m.hasTargets || c->hasPresence()) c->presence().setTargets(std::move(targets));
    }
    if (m.hasTargets && c->hasPresence()) c->presence().want(a.now);
    for (const ListRow& r : m.rows)
      for (const auto& mb : r.members)
        if (mb.presence == PresenceState::Checking) m.checking = true;
    for (const ListRow& r : m.rows)
      if (!r.workingNow.empty()) ++m.live;
  }
  // entries: Start (actions), the projects, the settings
  const bool searching = search && *search;
  if (!searching) {
    m.entries.push_back({Kind::Section, -1, tr("Start")});
    m.entries.push_back({Kind::Join, -1, ""});
    m.entries.push_back({Kind::New, -1, ""});
    if (!m.openName.empty()) m.entries.push_back({Kind::Share, -1, ""});
  }
  int shown = 0;
  for (size_t i = 0; i < m.rows.size(); ++i)
    if (matches(m.rows[i], search)) ++shown;
  if (!searching || shown > 0) m.entries.push_back({Kind::Section, -1, tr("Projects on this computer")});
  for (size_t i = 0; i < m.rows.size(); ++i)
    if (matches(m.rows[i], search)) m.entries.push_back({Kind::Project, static_cast<int>(i), ""});
  if (!searching && m.rows.empty()) m.entries.push_back({Kind::Empty, -1, ""});
  if (!searching) {
    m.entries.push_back({Kind::Section, -1, tr("Collaboration settings")});
    m.entries.push_back({Kind::Settings, -1, ""});
  }
}

// ------------------------------------------------------------------ what a tile says
struct Status {
  std::string text;
  ImU32 col = 0, dot = 0;
  bool pulse = false;
};

Status statusOf(const ListRow& r, const Look& C) {
  const auto& P = theme::pal();
  const int64_t now = wallNow();
  Status s;
  s.col = C.dim;
  const std::string by = r.byName.empty() ? std::string(tr("The admin")) : r.byName;
  if (r.state == St::Removed || r.removedNotice) return {fmtS(tr("%s removed you"), by), C.dim, 0, false};
  if (r.state == St::Ended) return {fmtS(tr("%s stopped sharing it"), by), C.dim, 0, false};
  if (r.state == St::Missing) return {tr("Folder not found. Moved or renamed?"), theme::withAlpha(C.danger, 0.95f), 0, false};
  if (!r.needVersion.empty()) return {fmtS(tr("Needs Gridline %s"), r.needVersion), theme::lit(P.hot), P.hot, false};
  if (r.review > 0 && r.admin)
    return {r.review == 1 ? std::string(tr("1 change to review")) : fmtN(tr("%lld changes to review"), static_cast<long long>(r.review)), theme::lit(P.hot), P.hot,
            false};
  if (!r.workingNow.empty()) {
    const std::string names = joinNames(r.workingNow, tr("and"), tr("%d others"));
    return {fmtS(r.workingNow.size() == 1 ? tr("%s is working on it now") : tr("%s are working on it now"), names), C.text, P.led, true};
  }
  if (r.openHere) return {tr("Open here"), C.dim, C.accent, false};
  if (r.pending > 0)
    return {r.pending == 1 ? std::string(tr("1 change not sent yet")) : fmtN(tr("%lld changes not sent yet"), static_cast<long long>(r.pending)), C.dim, 0,
            false};
  if (r.lastSeenAt > 0) return {fmtS(tr("Someone was here %s"), ago(r.lastSeenAt, now)), C.dim, 0, false};
  if (r.unreachable) return {tr("Nobody reachable right now"), C.faint, 0, false};
  if (r.lastSyncMs > 0) return {fmtS(tr("Synced %s"), ago(static_cast<int64_t>(r.lastSyncMs / 1000), now)), C.dim, 0, false};
  if (!r.hasCard) return {tr("Open it once to see its members"), C.faint, 0, false};
  return {r.members.size() == 1 ? std::string(tr("Only you so far")) : fmtN(tr("%lld members"), static_cast<long long>(r.members.size())), C.dim, 0, false};
}

std::string memberLine(const ListRow& r, const ListMember& m) {
  const int64_t now = wallNow();
  if (m.me) return r.openHere ? tr("You, open here") : tr("You");
  std::string s;
  switch (m.presence) {
    case PresenceState::Working:
      if (m.live) s = tr("Connected now");
      else if (m.now) s = m.since > 0 ? fmtS(tr("Working on it now, since %s"), clockText(m.since)) : std::string(tr("Working on it now"));
      else s = fmtS(tr("Was working on it %s"), ago(m.checkedAt, now));
      if (m.now && m.idle >= 300) s += "  \xC2\xB7  " + fmtN(tr("idle %lld min"), static_cast<long long>(m.idle / 60));
      if (m.now && !m.viaName.empty()) s += "  \xC2\xB7  " + fmtS(tr("seen by %s"), m.viaName);
      break;
    case PresenceState::OtherVersion: s = fmtS(tr("Has it open in Gridline %s"), m.app); break;
    case PresenceState::Online: s = tr("Gridline is open, not this project"); break;
    case PresenceState::Hidden: s = tr("Keeps presence private"); break;
    case PresenceState::Checking: s = tr("Checking..."); break;
    case PresenceState::NoAnswer:
      s = m.live ? std::string(tr("Not connected")) : std::string(tr("Not reachable right now"));
      break;
    default:
      s = m.live ? std::string(tr("Not connected")) : r.openHere || r.state != St::Active ? std::string() : std::string(tr("Not checked"));
      break;
  }
  if (!m.now && m.seenAt > 0) s += (s.empty() ? "" : "  \xC2\xB7  ") + fmtS(tr("last seen %s"), ago(m.seenAt, now));
  if (m.mineElsewhere) s = std::string(tr("You, on another computer")) + (s.empty() ? "" : "  \xC2\xB7  " + s);
  return s;
}

// A small rounded label (mono caps): returns its width.
float pill(App& a, ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, ImU32 fg, ImU32 dot = 0, float dotAlpha = 1.0f, ImU32 border = 0) {
  const float h = 18, fs = 10;
  const ImVec2 ts = measure(a.fontMono, fs, text);
  const float dotW = dot ? 10.0f : 0.0f;
  const float w = std::round(ts.x + 14 + dotW);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
  if (border) dl->AddRect(p, ImVec2(p.x + w, p.y + h), border, h * 0.5f);
  if (dot) dl->AddCircleFilled(ImVec2(p.x + 10, p.y + h * 0.5f), 3.2f, theme::withAlpha(dot, dotAlpha), 12);
  dl->AddText(a.fontMono, fs, ImVec2(std::round(p.x + 7 + dotW), std::round(p.y + (h - ts.y) * 0.5f)), fg, text);
  return w;
}

float pulse(App& a) {
  if (!scrollui::animations()) return 1.0f;
  return 0.55f + 0.45f * (0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 3.2f));
}

// The members' pictures, overlapping; those working on it now ringed. Returns the hovered member (-1 none).
int avatarStack(App& a, ImDrawList* dl, const ListRow& r, ImVec2 p, float rad, int maxShown, const Look& C, ImU32 cut, bool hoverable) {
  const auto& P = theme::pal();
  std::vector<int> order;
  for (int pass = 0; pass < 3; ++pass)
    for (size_t i = 0; i < r.members.size(); ++i) {
      const ListMember& m = r.members[i];
      const int want = m.me ? 2 : m.now ? 0 : 1;
      if (want == pass) order.push_back(static_cast<int>(i));
    }
  const int n = static_cast<int>(order.size());
  const int shown = n > maxShown ? maxShown - 1 : n;
  const float step = rad * 1.55f;
  int hovered = -1;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (int k = 0; k < shown; ++k) {
    const ListMember& m = r.members[static_cast<size_t>(order[static_cast<size_t>(k)])];
    const ImVec2 c(p.x + rad + step * static_cast<float>(k), p.y + rad);
    dl->AddCircleFilled(c, rad + 2.0f, cut, 24);
    const auto pit = M().pictures.find(m.fingerprint);
    const std::string& jpeg = m.me ? M().myPicture : pit != M().pictures.end() ? pit->second : std::string();
    collabui::drawPersonAvatar(a, dl, c, rad, m.name, colorFor(m), jpeg, -1);
    if (m.now) {
      dl->AddCircle(c, rad + 1.5f, P.led, 28, 2.0f);
      const ImVec2 d(c.x + rad * 0.72f, c.y + rad * 0.72f);
      dl->AddCircleFilled(d, std::max(3.0f, rad * 0.32f), cut, 12);
      dl->AddCircleFilled(d, std::max(2.2f, rad * 0.22f), P.led, 12);
    }
    if (hoverable && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && std::hypot(mouse.x - c.x, mouse.y - c.y) <= rad + 1)
      hovered = order[static_cast<size_t>(k)];
  }
  if (n > shown) {
    const ImVec2 c(p.x + rad + step * static_cast<float>(shown), p.y + rad);
    dl->AddCircleFilled(c, rad + 2.0f, cut, 24);
    dl->AddCircleFilled(c, rad, theme::mix(C.card, C.text, 0.16f), 24);
    const std::string more = "+" + std::to_string(n - shown);
    const ImVec2 ts = measure(a.fontBold, std::max(9.0f, rad * 0.85f), more.c_str());
    dl->AddText(a.fontBold, std::max(9.0f, rad * 0.85f), ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), C.text, more.c_str());
  }
  return hovered;
}

// The tile's tooltip: the project, my role, every member with what is known about them.
void projectTooltip(App& a, const ListRow& r, int onlyMember) {
  const auto& P = theme::pal();
  {  // kept inside the window (a tooltip that leaves it would become a window of its own)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 m = ImGui::GetIO().MousePos, sz = M().tipSize;
    ImVec2 pos(m.x + 16, m.y + 18);
    if (pos.x + sz.x > vp->Pos.x + vp->Size.x - 8) pos.x = std::max(vp->Pos.x + 8, m.x - 12 - sz.x);
    if (pos.y + sz.y > vp->Pos.y + vp->Size.y - 8) pos.y = std::max(vp->Pos.y + 8, m.y - 12 - sz.y);
    ImGui::SetNextWindowPos(pos);
  }
  if (!ImGui::BeginTooltip()) return;
  const float wrap = 300;
  if (onlyMember < 0) {
    ImGui::PushFont(a.fontBold, theme::kFontBody);
    ImGui::TextUnformatted(r.name.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(r.folder.c_str());
    std::string role = r.admin ? tr("You are its admin") : tr("You are a member");
    if (r.lastSyncMs > 0) role += "  \xC2\xB7  " + fmtS(tr("synced %s"), ago(static_cast<int64_t>(r.lastSyncMs / 1000), wallNow()));
    ImGui::TextUnformatted(role.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (!r.members.empty()) ImGui::Dummy(ImVec2(1, 4));
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < r.members.size(); ++i) {
    if (onlyMember >= 0 && static_cast<int>(i) != onlyMember) continue;
    const ListMember& m = r.members[i];
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rad = 9;
    const auto pit = M().pictures.find(m.fingerprint);
    collabui::drawPersonAvatar(a, dl, ImVec2(p.x + rad, p.y + rad + 2), rad, m.name, colorFor(m),
                               m.me ? M().myPicture : pit != M().pictures.end() ? pit->second : std::string(), m.now ? 1 : -1);
    ImGui::SetCursorScreenPos(ImVec2(p.x + rad * 2 + 8, p.y));
    ImGui::BeginGroup();
    std::string name = m.name.empty() ? std::string(tr("Someone")) : m.name;
    if (m.admin) name += std::string("  \xC2\xB7  ") + tr("admin");
    ImGui::TextUnformatted(name.c_str());
    const std::string line = memberLine(r, m);
    if (!line.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(m.now ? theme::lit(P.led) : P.textDim));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap - 30);
      ImGui::TextUnformatted(line.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 2));
  }
  if (onlyMember < 0 && r.state == St::Active && !r.openHere && r.needVersion.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(M().presenceOn ? tr("No answer does not mean offline: some networks block the check.")
                                          : tr("Presence is off: turn on \"Show that I am online\" below to see who is here."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  M().tipSize = ImGui::GetWindowSize();
  ImGui::EndTooltip();
}

// ------------------------------------------------------------------ tiles
struct TileFrame {
  ImVec2 p0, p1, t0, t1;
  float ht = 0, r = 0;
  bool hov = false, held = false, activated = false, tip = false;
};

TileFrame beginTile(App& a, int i, ImVec2 p, const launcher::Layout& L, const Look& C, bool focused, bool keyNav) {
  TileFrame f;
  f.p0 = p;
  f.p1 = ImVec2(p.x + L.tileW, p.y + L.tileH);
  ImGui::SetCursorScreenPos(p);
  ImGui::SetNextItemAllowOverlap();
  f.activated = ImGui::InvisibleButton("##tile", ImVec2(L.tileW, L.tileH));
  f.hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
  f.held = ImGui::IsItemActive();
  if (f.hov && M().tipId != ImGui::GetItemID()) M().tipId = ImGui::GetItemID(), M().tipSince = a.now;
  else if (!f.hov && M().tipId == ImGui::GetItemID()) M().tipId = 0;
  f.tip = f.hov && a.now - M().tipSince >= 0.45;
  f.ht = animate(ImGui::GetID("##hov"), f.hov || (focused && keyNav));
  f.r = theme::kRadiusLg + 2;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(f.p0, f.p1, theme::mix(C.card, C.cardHover, f.ht), f.r);
  f.t0 = ImVec2(p.x + 1, p.y + 1);
  f.t1 = ImVec2(f.p1.x - 1, p.y + L.thumbH);
  (void)a;
  (void)i;
  return f;
}

void endTile(const TileFrame& f, const Look& C, bool focused, bool keyNav, ImU32 edgeOverride = 0) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(f.t0.x, f.t1.y), ImVec2(f.t1.x, f.t1.y), theme::withAlpha(C.line, 0.8f));
  ImU32 edge = theme::mix(C.line, C.lineHover, f.ht);
  if (edgeOverride) edge = theme::withAlpha(edgeOverride, 0.75f + 0.25f * f.ht);
  dl->AddRect(f.p0, f.p1, edge, f.r, 0, edgeOverride ? 1.5f : 1.0f);
  if (focused && keyNav) dl->AddRect(ImVec2(f.p0.x - 3, f.p0.y - 3), ImVec2(f.p1.x + 3, f.p1.y + 3), C.accent, f.r + 3, 0, 2.0f);
  if (f.held) dl->AddRectFilled(f.p0, f.p1, theme::withAlpha(P.ink, P.dark ? 0.10f : 0.04f), f.r);
}

void caption(App& a, const TileFrame& f, const launcher::Layout& L, const Look& C, const char* name, const char* meta, ImU32 metaCol) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = f.p0.x + 14, nameY = f.p0.y + L.thumbH + 13;
  textAt(dl, a.fontBold, theme::kFontBody, ImVec2(cx, nameY), C.text, name, L.tileW - 28);
  textAt(dl, a.fontRegular, theme::kFontCaption, ImVec2(cx, nameY + 21), metaCol ? metaCol : C.dim, meta, L.tileW - 28);
}

bool drawJoin(App& a, int i, ImVec2 p, const launcher::Layout& L, bool focused, bool keyNav, const Look& C) {
  const auto& P = theme::pal();
  TileFrame f = beginTile(a, i, p, L, C, focused, keyNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = theme::mix(C.thumb, C.cardHover, 0.35f + 0.25f * f.ht);
  dl->AddRectFilled(f.t0, f.t1, bg, f.r - 1, ImDrawFlags_RoundCornersTop);
  // an invite code, as it looks when pasted: groups of letters, the middle line lit
  dl->PushClipRect(f.t0, f.t1, true);
  const float fs = std::clamp(L.thumbH * 0.095f, 10.0f, 15.0f), lh = fs * 1.55f;
  static const char* const kLines[] = {"GLX1 7QK4 F9M2 PX3T", "W8RD 2KCN V6LH Q0ZJ", "M4TB Y7EH 9SUA K2DF", "R5XN H3PW 8GQC T1VE"};
  const float blockW = measure(a.fontMono, fs, kLines[0]).x;
  const float x0 = std::round((f.t0.x + f.t1.x - blockW) * 0.5f), y0 = std::round((f.t0.y + f.t1.y) * 0.5f - lh * 2.0f);
  for (int k = 0; k < 4; ++k) {
    const ImU32 col = k == 1 ? theme::mix(C.dim, C.accent, 0.35f + 0.4f * f.ht) : theme::withAlpha(C.faint, k == 3 ? 0.35f : 0.6f);
    dl->AddText(a.fontMono, fs, ImVec2(x0, std::round(y0 + lh * static_cast<float>(k))), col, kLines[k]);
  }
  // the caret after the lit line
  const float cxp = x0 + measure(a.fontMono, fs, kLines[1]).x + 3;
  if (!scrollui::animations() || std::fmod(a.now, 1.0) < 0.6)
    dl->AddRectFilled(ImVec2(cxp, y0 + lh + 1), ImVec2(cxp + 1.5f, y0 + lh + fs + 1), C.accent);
  dl->PopClipRect();
  (void)P;
  caption(a, f, L, C, tr("Join with a code"), tr("Paste the invite a collaborator sent you"), 0);
  endTile(f, C, focused, keyNav);
  return f.activated;
}

bool drawNew(App& a, int i, ImVec2 p, const launcher::Layout& L, bool focused, bool keyNav, const Look& C) {
  TileFrame f = beginTile(a, i, p, L, C, focused, keyNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = theme::mix(C.thumb, C.cardHover, 0.35f + 0.25f * f.ht);
  dl->AddRectFilled(f.t0, f.t1, bg, f.r - 1, ImDrawFlags_RoundCornersTop);
  const ImVec2 mc((f.t0.x + f.t1.x) * 0.5f, (f.t0.y + f.t1.y) * 0.5f);
  const float rad = std::round(std::clamp(L.thumbH * 0.2f, 18.0f, 36.0f));
  dl->AddCircle(mc, rad, theme::withAlpha(C.line, 0.9f), 48, 1.5f);
  ui::drawIcon(dl, ui::Icon::People, mc, std::round(rad * 1.05f), theme::mix(C.dim, C.text, f.ht));
  const ImVec2 pc(mc.x + rad * 0.74f, mc.y + rad * 0.74f);
  const float pr = std::max(9.0f, rad * 0.36f);
  dl->AddCircleFilled(pc, pr + 2, bg, 24);
  dl->AddCircleFilled(pc, pr, C.accent, 24);
  ui::drawIcon(dl, ui::Icon::Plus, pc, std::round(pr * 1.3f), theme::contrastText(C.accent));
  caption(a, f, L, C, tr("New collaborative project"), tr("Start empty, then invite people"), 0);
  endTile(f, C, focused, keyNav);
  return f.activated;
}

bool drawShare(App& a, int i, ImVec2 p, const launcher::Layout& L, bool focused, bool keyNav, const Look& C) {
  TileFrame f = beginTile(a, i, p, L, C, focused, keyNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(f.t0, f.t1, C.thumb, f.r - 1, ImDrawFlags_RoundCornersTop);
  dl->PushClipRect(f.t0, f.t1, true);
  launcherui::drawSketchKey(a, dl, f.t0, f.t1, "@current", 0.55f + 0.2f * f.ht);
  dl->PopClipRect();
  const ImVec2 bc(f.t1.x - 24, f.t1.y - 24);
  dl->AddCircleFilled(bc, 15, C.accent, 32);
  ui::drawIcon(dl, ui::Icon::People, bc, 16, theme::contrastText(C.accent));
  caption(a, f, L, C, tr("Share the open project"), fmtS(tr("Make %s collaborative"), M().openName).c_str(), 0);
  endTile(f, C, focused, keyNav);
  return f.activated;
}

void openMenuFor(const ListRow& r) {
  M().menu = r;
  M().openMenu = true;
}

// Favourite collaborative projects: their ids, one per line, in the setting "collab.favourites"
// (the project id stays when its folder moves).
std::vector<std::string> favouriteIds(const App& a) {
  std::vector<std::string> out;
  const std::string s = a.settings.getString("collab.favourites");
  size_t at = 0;
  while (at < s.size()) {
    const size_t nl = s.find('\n', at);
    const std::string id = s.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    if (!id.empty()) out.push_back(id);
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  return out;
}
bool isFavourite(const App& a, const std::string& id) {
  const auto ids = favouriteIds(a);
  return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end();
}
void setFavourite(App& a, const std::string& id, bool on) {
  if (id.empty()) return;
  auto ids = favouriteIds(a);
  ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
  if (on) ids.push_back(id);
  std::string s;
  for (const auto& x : ids) s += x + "\n";
  a.settings.set("collab.favourites", s);
}

bool drawProject(App& a, int i, const ListRow& r, ImVec2 p, const launcher::Layout& L, bool focused, bool keyNav, const Look& C) {
  const auto& P = theme::pal();
  TileFrame f = beginTile(a, i, p, L, C, focused, keyNav);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) openMenuFor(r);
  M().tileRects[r.id] = ImVec4(f.p0.x, f.p0.y, f.p1.x, f.p1.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool gone = r.state != St::Active || r.removedNotice;
  const float dim = gone ? 0.5f : 1.0f;
  const ImU32 cardNow = theme::mix(C.card, C.cardHover, f.ht);
  // thumbnail: the arrangement sketch
  dl->AddRectFilled(f.t0, f.t1, theme::withAlpha(C.thumb, gone ? 0.6f : 1.0f), f.r - 1, ImDrawFlags_RoundCornersTop);
  if (r.state == St::Missing) {
    const char* nf = tr("Not found");
    const ImVec2 ts = measure(a.fontMono, theme::kFontLabel, nf);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round((f.t0.x + f.t1.x - ts.x) * 0.5f), std::round((f.t0.y + f.t1.y - ts.y) * 0.5f)),
                theme::withAlpha(C.danger, 0.9f), nf);
  } else {
    dl->PushClipRect(f.t0, f.t1, true);
    const float sketchBottom = f.t1.y - std::round(std::clamp(L.thumbH * 0.34f, 40.0f, 56.0f)) * 0.5f;  // above the members band
    launcherui::drawSketchKey(a, dl, ImVec2(f.t0.x, f.t0.y + 8), ImVec2(f.t1.x, sketchBottom), r.openHere ? std::string("@current") : r.projectFile, dim);
    dl->PopClipRect();
  }
  // a calm band at the bottom of the thumbnail for the members and the chips
  {
    const float band = std::round(std::clamp(L.thumbH * 0.34f, 40.0f, 56.0f));
    const ImU32 solid = theme::withAlpha(C.thumb, 0.94f), clear = theme::withAlpha(C.thumb, 0.0f);
    dl->AddRectFilledMultiColor(ImVec2(f.t0.x, f.t1.y - band), ImVec2(f.t1.x, f.t1.y - band * 0.45f), clear, clear, solid, solid);
    dl->AddRectFilled(ImVec2(f.t0.x, f.t1.y - band * 0.45f), f.t1, solid);
  }
  // a favourite wears a heart badge in the thumbnail's top-left corner (as on the Recent tiles), the pills follow it
  const bool fav = isFavourite(a, r.id);
  const float heartD = 26;
  if (fav) {
    const ImVec2 h0(f.t0.x + 8, f.t0.y + 8);
    dl->AddRectFilled(h0, ImVec2(h0.x + heartD, h0.y + heartD), theme::withAlpha(C.card, 0.92f), heartD * 0.5f);
    ui::drawIcon(dl, ui::Icon::HeartFilled, ImVec2(h0.x + heartD * 0.5f, h0.y + heartD * 0.5f + 0.5f), 14, C.heart);
  }
  // pills, top left: live / open here / state
  {
    float x = f.t0.x + 10 + (fav ? heartD + 4 : 0.0f);
    const float y = f.t0.y + 10;
    const ImU32 glass = theme::withAlpha(C.card, 0.92f);
    if (!r.workingNow.empty() && !gone) x += pill(a, dl, ImVec2(x, y), tr("LIVE"), glass, C.text, P.led, pulse(a), theme::withAlpha(P.led, 0.45f)) + 6;
    if (r.openHere) x += pill(a, dl, ImVec2(x, y), tr("OPEN HERE"), C.accent, theme::contrastText(C.accent)) + 6;
    if (r.state == St::Removed || r.removedNotice) x += pill(a, dl, ImVec2(x, y), tr("REMOVED"), glass, C.dim, 0, 1, C.line) + 6;
    else if (r.state == St::Ended) x += pill(a, dl, ImVec2(x, y), tr("NOT SHARED"), glass, C.dim, 0, 1, C.line) + 6;
    else if (r.state == St::Missing) x += pill(a, dl, ImVec2(x, y), tr("MISSING"), theme::mix(C.card, C.danger, 0.18f), theme::lit(C.danger), 0, 1, theme::withAlpha(C.danger, 0.5f)) + 6;
    else if (!r.needVersion.empty()) x += pill(a, dl, ImVec2(x, y), tr("UPDATE NEEDED"), theme::mix(C.card, P.hot, 0.18f), theme::lit(P.hot), 0, 1, theme::withAlpha(P.hot, 0.5f)) + 6;
    (void)x;
  }
  // chips, bottom right: what waits (review, unsent changes)
  {
    float x = f.t1.x - 10;
    const float y = f.t1.y - 10 - 18;
    const ImU32 glass = theme::withAlpha(C.card, 0.92f);
    auto chip = [&](const std::string& t, ImU32 fg, ImU32 border) {
      const float w = std::round(measure(a.fontMono, 10, t.c_str()).x + 14);
      x -= w;
      pill(a, dl, ImVec2(x, y), t.c_str(), glass, fg, 0, 1, border);
      x -= 6;
    };
    if (!gone && r.pending > 0)
      chip(fmtN(tr("%lld UNSENT"), static_cast<long long>(r.pending)), C.dim, C.line);
    if (!gone && r.review > 0 && r.admin) chip(fmtN(tr("%lld TO REVIEW"), static_cast<long long>(r.review)), theme::lit(P.hot), theme::withAlpha(P.hot, 0.5f));
  }
  // members, bottom left of the thumbnail
  int hoveredMember = -1;
  if (!r.members.empty()) {
    const float rad = std::round(std::clamp(L.thumbH * 0.085f, 10.0f, 14.0f));
    const int maxShown = L.tileW < 260 ? 4 : 5;
    hoveredMember = avatarStack(a, dl, r, ImVec2(f.t0.x + 10, f.t1.y - 10 - rad * 2), rad, maxShown, C, C.thumb, f.hov);
  }
  // caption: name, role, status
  const float cx = f.p0.x + 14, nameY = f.p0.y + L.thumbH + 13;
  float cw = L.tileW - 28;
  if (r.admin && r.state == St::Active) {
    const char* b = tr("ADMIN");
    const float w = std::round(measure(a.fontMono, 10, b).x + 14);
    pill(a, dl, ImVec2(f.p1.x - 12 - w, nameY - 1), b, theme::withAlpha(cardNow, 0.0f), C.dim, 0, 1, C.line);
    cw -= w + 8;
  }
  textAt(dl, a.fontBold, theme::kFontBody, ImVec2(cx, nameY), theme::withAlpha(C.text, gone ? 0.7f : 1.0f), r.name.c_str(), cw);
  const Status st = statusOf(r, C);
  float sx = cx;
  if (st.dot) {
    const float da = st.pulse ? pulse(a) : 1.0f;
    dl->AddCircleFilled(ImVec2(cx + 3.5f, nameY + 21 + theme::kFontCaption * 0.5f + 1), 3.5f, theme::withAlpha(st.dot, da), 12);
    sx += 12;
  }
  textAt(dl, a.fontRegular, theme::kFontCaption, ImVec2(sx, nameY + 21), st.col, st.text.c_str(), L.tileW - 28 - (sx - cx));
  endTile(f, C, focused, keyNav, r.openHere ? C.accent : (!r.workingNow.empty() && !gone ? theme::mix(C.line, P.led, 0.55f) : 0));
  // hover: the heart and menu buttons, the tooltip. The buttons stay while pressed: the tile
  // reports no hover while one of its buttons is the active item, and a button that is not
  // submitted on the frame the mouse is released never reports the click.
  bool overMenu = false;
  const ImGuiID moreId = ImGui::GetID("##more"), favId = ImGui::GetID("##fav"), activeId = ImGui::GetActiveID();
  const bool menuOpen = ImGui::IsPopupOpen("##collabTileMenu") && M().menu.id == r.id && M().menu.folder == r.folder;
  if (f.hov || menuOpen || activeId == moreId || activeId == favId) {
    const float bs = 26;
    const ImVec2 m0(f.t1.x - bs - 8, f.t0.y + 8);
    ImGui::SetCursorScreenPos(m0);
    if (ImGui::InvisibleButton("##more", ImVec2(bs, bs))) openMenuFor(r);
    const bool mh = ImGui::IsItemHovered();
    if (mh) ImGui::SetTooltip("%s", tr("More"));
    dl->AddRectFilled(m0, ImVec2(m0.x + bs, m0.y + bs), theme::withAlpha(mh ? C.cardHover : C.card, 0.92f), theme::kRadius + 1);
    ui::drawIcon(dl, ui::Icon::More, ImVec2(m0.x + bs * 0.5f, m0.y + bs * 0.5f), 14, mh ? C.text : C.dim);
    bool fh = false;
    if (!fav && !r.id.empty()) {
      const ImVec2 f0(m0.x - bs - 6, m0.y);
      ImGui::SetCursorScreenPos(f0);
      if (ImGui::InvisibleButton("##fav", ImVec2(bs, bs))) setFavourite(a, r.id, true);
      fh = ImGui::IsItemHovered();
      if (fh) ImGui::SetTooltip("%s", tr("Add to favourites"));
      dl->AddRectFilled(f0, ImVec2(f0.x + bs, f0.y + bs), theme::withAlpha(fh ? C.cardHover : C.card, 0.92f), theme::kRadius + 1);
      ui::drawIcon(dl, fh ? ui::Icon::HeartFilled : ui::Icon::Heart, ImVec2(f0.x + bs * 0.5f, f0.y + bs * 0.5f + 0.5f), 14, fh ? C.heart : C.dim);
    }
    overMenu = mh || fh;
    if (overMenu) f.activated = false;
  }
  // the heart badge doubles as the "remove from favourites" button
  if (fav) {
    ImGui::SetCursorScreenPos(ImVec2(f.t0.x + 8, f.t0.y + 8));
    if (ImGui::InvisibleButton("##unfav", ImVec2(heartD, heartD))) setFavourite(a, r.id, false);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tr("Remove from favourites"));
      overMenu = true;
      f.activated = false;
    }
  }
  if (!overMenu && hoveredMember >= 0) projectTooltip(a, r, hoveredMember);  // one member
  else if (!overMenu && f.tip) projectTooltip(a, r, -1);                     // the whole project
  return f.activated;
}

void drawSection(App& a, ImDrawList* dl, ImVec2 p, const launcher::Rect& r, const std::string& name, const Look& C) {
  std::string up = name;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const float y = std::round(p.y + r.h - 22);
  const float upW = measure(a.fontMono, theme::kFontLabel, up.c_str()).x;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p.x + 2), y), C.faint, up.c_str());
  if (name == tr("Collaboration settings")) {  // where else it lives
    const char* note = tr("Also in Settings > Collaboration");
    const float w = measure(a.fontRegular, theme::kFontCaption, note).x;
    if (r.w - upW > w + 40) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(p.x + std::min(r.w, 860.0f) - w - 2), y - 1), C.faint, note);
  } else if (name == tr("Projects on this computer") && !M().rows.empty()) {
    char n[16];
    std::snprintf(n, sizeof n, "%d", static_cast<int>(M().rows.size()));
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p.x + 2 + upW + 10), y), theme::withAlpha(C.faint, 0.7f), n);
  }
}

void drawEmpty(App& a, ImDrawList* dl, ImVec2 p, const Look& C) {
  dl->AddText(a.fontBold, theme::kFontBody, ImVec2(std::round(p.x + 2), std::round(p.y + 6)), C.dim, tr("No collaborative projects on this computer yet."));
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(p.x + 2), std::round(p.y + 28)), C.faint,
              tr("Join one with the code a collaborator sends you, or make one and invite people."));
}

void drawSettings(App& a, ImVec2 p, const launcher::Rect& r) {
  Model& m = M();
  const float w = std::min(r.w, 860.0f);
  ImGui::SetCursorScreenPos(p);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::BeginChild("##collabSettings", ImVec2(w, std::max(40.0f, r.h)), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground)) {
    const float fieldW = std::max(160.0f, std::min(320.0f, w - 176 - 2 * theme::kS2 - 12));
    collabui::settingsPage(a, fieldW);
    m.settingsH = std::ceil(ImGui::GetCursorPosY() + 4);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void projectMenu(App& a) {
  Model& m = M();
  if (m.openMenu) {
    ImGui::OpenPopup("##collabTileMenu");
    m.openMenu = false;
  }
  m.menuShown = false;
  if (!ImGui::BeginPopup("##collabTileMenu")) return;
  m.menuShown = true;
  const ListRow r = m.menu;
  using A = collabui::ProjectAction;
  const int state = r.state == St::Active ? 0 : r.state == St::Removed ? 1 : r.state == St::Ended ? 2 : 3;
  auto act = [&](A x) { collabui::projectAction(a, x, r.id, r.folder, r.name, state); };
  ImGui::TextDisabled("%s", r.name.c_str());
  ImGui::Separator();
  if (!r.id.empty()) {
    const bool fav = isFavourite(a, r.id);
    if (ImGui::MenuItem(fav ? tr("Remove from favourites") : tr("Add to favourites"), "F")) setFavourite(a, r.id, !fav);
    ImGui::Separator();
  }
  if (r.state == St::Active && !r.removedNotice) {
    const bool usable = r.needVersion.empty();
    if (ImGui::MenuItem(r.openHere ? tr("Back to project") : tr("Open"))) act(A::Open);
    if (r.admin) {
      if (ImGui::MenuItem(tr("Invite someone..."), nullptr, false, usable)) act(A::Invite);
      if (ImGui::MenuItem(tr("Reconnect codes..."), nullptr, false, usable)) act(A::Reconnect);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(tr(revealLabel()))) act(A::Reveal);
    if (ImGui::MenuItem(tr("Copy folder path"))) act(A::CopyPath);
    ImGui::Separator();
    if (r.admin) {
      if (ImGui::MenuItem(tr("Stop sharing..."), nullptr, false, usable)) act(A::StopSharing);
    } else if (ImGui::MenuItem(tr("Leave project..."), nullptr, false, usable)) {
      act(A::Leave);
    }
    if (ImGui::MenuItem(tr("Remove from this list..."), nullptr, false, !r.openHere)) act(A::Forget);
  } else if (r.state == St::Missing) {
    if (ImGui::MenuItem(tr("Locate..."))) act(A::Locate);
    if (ImGui::MenuItem(tr("Copy folder path"))) act(A::CopyPath);
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Forget..."))) act(A::Forget);
  } else {
    if (ImGui::MenuItem(tr("Open"))) act(A::Open);
    if (ImGui::MenuItem(tr("Keep a private copy..."))) act(A::KeepCopy);
    if (ImGui::MenuItem(tr(revealLabel()))) act(A::Reveal);
    if (ImGui::MenuItem(tr("Copy folder path"))) act(A::CopyPath);
    if (r.state != St::Active) {
      ImGui::Separator();
      if (ImGui::MenuItem(tr("Forget..."))) act(A::Forget);
    }
  }
  ImGui::EndPopup();
}

const char* noteText() {
  const Model& m = M();
  if (!m.presenceOn) return tr("PRESENCE OFF");
  if (m.live > 0) return nullptr;  // drawn with its count
  if (m.checking) return tr("CHECKING");
  return "";
}

std::string liveText() { return M().live == 1 ? std::string(tr("1 LIVE")) : fmtN(tr("%lld LIVE"), static_cast<long long>(M().live)); }

}  // namespace

// ------------------------------------------------------------------ public
void update(App& a, const char* search) { refresh(a, search); }

void items(std::vector<launcher::Item>& items, std::vector<float>& blockH) {
  const Model& m = M();
  items.clear();
  blockH.clear();
  for (const Entry& e : m.entries) {
    switch (e.kind) {
      case Kind::Section: items.push_back(launcher::Item::Section); break;
      case Kind::Empty:
        items.push_back(launcher::Item::Block);
        blockH.push_back(56);
        break;
      case Kind::Settings:
        items.push_back(launcher::Item::Block);
        blockH.push_back(m.settingsH);
        break;
      default: items.push_back(launcher::Item::Tile); break;
    }
  }
}

bool drawItem(App& a, int i, ImVec2 p, const launcher::Rect& r, const launcher::Layout& L, bool focused, bool keyNav, const Look& look) {
  Model& m = M();
  if (i < 0 || i >= static_cast<int>(m.entries.size())) return false;
  const Entry& e = m.entries[static_cast<size_t>(i)];
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(i);
  bool hit = false;
  switch (e.kind) {
    case Kind::Section: drawSection(a, dl, p, r, e.label, look); break;
    case Kind::Empty: drawEmpty(a, dl, p, look); break;
    case Kind::Settings: drawSettings(a, p, r); break;
    case Kind::Join: hit = drawJoin(a, i, p, L, focused, keyNav, look); break;
    case Kind::New: hit = drawNew(a, i, p, L, focused, keyNav, look); break;
    case Kind::Share: hit = drawShare(a, i, p, L, focused, keyNav, look); break;
    case Kind::Project:
      if (e.row >= 0 && e.row < static_cast<int>(m.rows.size())) hit = drawProject(a, i, m.rows[static_cast<size_t>(e.row)], p, L, focused, keyNav, look);
      break;
  }
  ImGui::PopID();
  return hit;
}

void activate(App& a, int i) {
  Model& m = M();
  if (i < 0 || i >= static_cast<int>(m.entries.size())) return;
  const Entry e = m.entries[static_cast<size_t>(i)];
  switch (e.kind) {
    case Kind::Join: collabui::joinWithCode(a, ""); break;
    case Kind::New: collabui::newCollaborative(a); break;
    case Kind::Share: collabui::shareOpenProject(a); break;
    case Kind::Project: {
      if (e.row < 0 || e.row >= static_cast<int>(m.rows.size())) break;
      const ListRow r = m.rows[static_cast<size_t>(e.row)];
      const int state = r.state == St::Active ? 0 : r.state == St::Removed ? 1 : r.state == St::Ended ? 2 : 3;
      // a moved folder: find it; anything else opens (a removed / stopped one explains itself and offers a private copy)
      collabui::projectAction(a, r.state == St::Missing ? collabui::ProjectAction::Locate : collabui::ProjectAction::Open, r.id, r.folder, r.name, state);
      break;
    }
    default: break;
  }
}

void openMenu(int i) {
  Model& m = M();
  if (i < 0 || i >= static_cast<int>(m.entries.size())) return;
  const Entry& e = m.entries[static_cast<size_t>(i)];
  if (e.kind == Kind::Project && e.row >= 0 && e.row < static_cast<int>(m.rows.size())) openMenuFor(m.rows[static_cast<size_t>(e.row)]);
}

void menus(App& a) { projectMenu(a); }

void toggleFavourite(App& a, int i) {
  Model& m = M();
  if (i < 0 || i >= static_cast<int>(m.entries.size())) return;
  const Entry& e = m.entries[static_cast<size_t>(i)];
  if (e.kind != Kind::Project || e.row < 0 || e.row >= static_cast<int>(m.rows.size())) return;
  const ListRow& r = m.rows[static_cast<size_t>(e.row)];
  setFavourite(a, r.id, !isFavourite(a, r.id));
}

int projectCount(App& a) {
  static int count = 0;
  static double at = -100;
  if (a.now - at > 2.0 || a.now < at) {
    at = a.now;
    // (no controller is made just for the count: someone who never collaborated gets no keys made)
    std::error_code ec;
    const bool any = a.collab || fs::exists(fs::path(a.configDir()) / "collab" / "known-projects.json", ec);
    collab::Controller* c = any ? collabui::controller(a) : nullptr;
    count = c ? static_cast<int>(c->knownProjects().size()) : 0;
  }
  return count;
}

int liveCount() { return M().live; }

float titleNoteWidth(App& a) {
  const char* t = noteText();
  if (t && !*t) return 0;
  const std::string s = t ? std::string(t) : liveText();
  return std::round(measure(a.fontMono, 10, s.c_str()).x + 14 + (t ? 0.0f : 10.0f));
}

void drawTitleNote(App& a, ImDrawList* dl, ImVec2 p, float h, const Look& look) {
  const auto& P = theme::pal();
  const char* t = noteText();
  if (t && !*t) return;
  const ImVec2 at(p.x, std::round(p.y + (h - 18) * 0.5f));
  if (!t) pill(a, dl, at, liveText().c_str(), look.fill, look.text, P.led, pulse(a), theme::withAlpha(P.led, 0.45f));
  else pill(a, dl, at, t, look.fill, look.faint, 0, 1, look.line);
  ImGui::SetCursorScreenPos(at);
  ImGui::InvisibleButton("##presenceNote", ImVec2(std::max(1.0f, titleNoteWidth(a)), 18));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ui::Tooltip(!M().presenceOn ? "Presence is off: collaborators do not see that you are working on a project, and you do not see them"
                                : "Who is working on your collaborative projects right now, checked directly with them while this view is open");
}

void devReport(App& a) {
  const Model& m = M();
  const auto& P = theme::pal();
  Look look{};
  look.dim = P.textDim;
  look.faint = P.textFaint;
  look.text = P.text;
  look.danger = P.danger;
  look.accent = P.accent;
  std::printf("collab view rows=%d live=%d probing=%d presence=%d targets=%d entries=%d join=%d panel=%d\n", static_cast<int>(m.rows.size()), m.live,
              m.probing ? 1 : 0, m.presenceOn ? 1 : 0, m.hasTargets ? 1 : 0, static_cast<int>(m.entries.size()), a.showJoin ? 1 : 0, a.showCollab ? 1 : 0);
  for (const ListRow& r : m.rows) {
    std::string members;
    for (const auto& mb : r.members) {
      if (!members.empty()) members += ",";
      members += mb.name + ":" + presenceStateName(mb.presence) + (mb.now ? "*" : "") + (mb.me ? "(me)" : "");
    }
    std::printf("collab row name=%s state=%d admin=%d open=%d card=%d pending=%d review=%d need=%s working=%d members=%s status=%s\n", r.name.c_str(),
                static_cast<int>(r.state), r.admin ? 1 : 0, r.openHere ? 1 : 0, r.hasCard ? 1 : 0, static_cast<int>(r.pending), static_cast<int>(r.review),
                r.needVersion.c_str(), static_cast<int>(r.workingNow.size()), members.c_str(), statusOf(r, look).text.c_str());
    const auto rc = m.tileRects.find(r.id);
    const ImVec4 t = rc != m.tileRects.end() ? rc->second : ImVec4(0, 0, 0, 0);
    std::printf("collab fav name=%s fav=%d tile=%.0f,%.0f,%.0f,%.0f\n", r.name.c_str(), isFavourite(a, r.id) ? 1 : 0, static_cast<double>(t.x),
                static_cast<double>(t.y), static_cast<double>(t.z), static_cast<double>(t.w));
  }
  std::printf("collab menu=%s\n", m.menuShown ? m.menu.name.c_str() : "-");
  std::fflush(stdout);
  (void)a;
}

}  // namespace gl::launchercollab

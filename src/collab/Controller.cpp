// Gridline - J-111 collaboration: controller. See Controller.h.
#include "Controller.h"
#include <algorithm>
#include <filesystem>
#include "../core/I18n.h"
#include "../core/Project.h"
#include "../core/Secrets.h"
#include "../core/Settings.h"
#include "GridlineVersion.h"
#include "Lifecycle.h"

namespace fs = std::filesystem;

namespace gl::collab {

NetSettings netSettingsFrom(const Settings& s) {
  NetSettings n;
  n.stunHost = s.getString("collab.stunHost");
  n.stunPort = static_cast<uint16_t>(std::clamp(s.getNumber("collab.stunPort"), 1.0, 65535.0));
  n.turnHost = s.getString("collab.turnHost");
  n.turnPort = static_cast<uint16_t>(std::clamp(s.getNumber("collab.turnPort"), 1.0, 65535.0));
  n.turnUser = s.getString("collab.turnUser");
  if (!n.turnHost.empty()) {
    secrets::Store st;
    st.get("collab-turn-password", n.turnPass);
  }
  n.portMapping = s.getBool("collab.portMapping");
  return n;
}

VersionInfo currentVersion() {
  VersionInfo v;
  v.app = GRIDLINE_VERSION;
  v.protocol = kProtocolVersion;
  v.build = GRIDLINE_BUILD_ID;
  return v;
}

namespace {
std::string knownPath(const ProfileStore& p) { return (fs::path(p.dir()) / "known-projects.json").string(); }
}  // namespace

namespace {
std::string normFolder(const std::string& f) { return fs::path(f).lexically_normal().string(); }
Json collabJsonOf(const std::string& folder) {
  std::string t;
  Json j;
  if (!readFileText((fs::path(folder) / ".gridline-collab" / "collab.json").string(), t, 1 << 20) || !jsonParse(t, j)) return Json();
  return j;
}
}  // namespace

std::vector<Controller::KnownProject> Controller::knownProjects() const {
  std::vector<KnownProject> out;
  std::string t;
  Json j;
  if (!readFileText(knownPath(profiles_), t, 1 << 20) || !jsonParse(t, j) || !j.is(Json::Type::Array)) return out;
  for (const auto& e : j.array) {
    KnownProject k;
    k.id = e.text("id");
    k.name = e.text("name");
    k.folder = e.text("folder");
    k.openedMs = static_cast<uint64_t>(e.num("opened", 0));
    if (k.id.empty() || k.folder.empty()) continue;
    const Json cj = collabJsonOf(k.folder);
    if (!cj.is(Json::Type::Object) || cj.text("id") != k.id) {
      k.state = KnownProject::State::Missing;  // moved, renamed or deleted outside Gridline
    } else if (cj.num("left", 0) != 0) {
      continue;
    } else if (const Json* r = cj.find("removed"); r && r->is(Json::Type::Object)) {
      k.state = KnownProject::State::Removed;
      k.byName = r->text("byName");
      k.reason = r->text("reason");
      k.at = static_cast<uint64_t>(r->num("at", 0));
    } else if (const Json* r2 = cj.find("ended"); r2 && r2->is(Json::Type::Object)) {
      k.state = KnownProject::State::Ended;
      k.byName = r2->text("byName");
      k.at = static_cast<uint64_t>(r2->num("at", 0));
    }
    out.push_back(k);
  }
  std::stable_sort(out.begin(), out.end(), [](const KnownProject& a, const KnownProject& b) { return a.openedMs > b.openedMs; });
  return out;
}

std::string Controller::knownFolder(const std::string& projectId) const {
  for (const auto& k : knownProjects())
    if (k.id == projectId && k.state != KnownProject::State::Missing) return k.folder;
  return {};
}

std::string Controller::joinTarget(const InviteInfo& info, const std::string& folder) const {
  if (Session::isCollabFolder(folder) && Session::projectIdOf(folder) == info.projectId) return folder;
  const std::string k = knownFolder(info.projectId);
  return k.empty() ? folder : k;
}

void Controller::writeKnown(const std::vector<KnownProject>& all) const {
  Json j = Json::makeArray();
  for (const auto& k : all) {
    Json e = Json::makeObject();
    e.object["id"] = Json::makeString(k.id);
    e.object["name"] = Json::makeString(k.name);
    e.object["folder"] = Json::makeString(k.folder);
    e.object["opened"] = Json::makeNumber(static_cast<double>(k.openedMs));
    j.array.push_back(std::move(e));
  }
  std::error_code ec;
  fs::create_directories(profiles_.dir(), ec);
  writeFileDurable(knownPath(profiles_), jsonStringify(j, 2));
}

void Controller::remember() {
  if (!session_) return;
  std::vector<KnownProject> all = knownProjects();
  const std::string folder = normFolder(session_->config().folder);
  all.erase(std::remove_if(all.begin(), all.end(), [&](const KnownProject& k) { return normFolder(k.folder) == folder || k.id == session_->projectId(); }), all.end());
  KnownProject me;
  me.id = session_->projectId();
  me.name = session_->projectName();
  me.folder = folder;
  me.openedMs = session_->wallNowMs();
  all.insert(all.begin(), me);
  if (all.size() > 30) all.resize(30);
  writeKnown(all);
}

void Controller::forgetProject(const std::string& folder) {
  std::vector<KnownProject> all = knownProjects();
  const std::string f = normFolder(folder);
  for (const auto& k : all)
    if (normFolder(k.folder) == f && (k.state == KnownProject::State::Removed || k.state == KnownProject::State::Ended)) {
      // nothing syncs here any more: the folder keeps the music as a normal project, without
      // the shared history and the other members' names
      Song song;
      if (loadProject(Session::projectFileOf(k.folder), song)) {
        std::string name = Session::projectNameOf(k.folder);
        exportPlainProject(song, k.folder, k.folder, name.empty() ? k.name : name, true, nullptr);
      }
      purgePrivate(k.id, {});
    }
  all.erase(std::remove_if(all.begin(), all.end(), [&](const KnownProject& k) { return normFolder(k.folder) == f; }), all.end());
  writeKnown(all);
}

bool Controller::locateProject(const std::string& projectId, const std::string& folder, std::string* err) {
  if (Session::projectIdOf(folder) != projectId) {
    if (err) *err = i18n::tr("That folder holds a different project.");
    return false;
  }
  std::vector<KnownProject> all = knownProjects();
  for (auto& k : all)
    if (k.id == projectId) k.folder = normFolder(folder);
  writeKnown(all);
  return true;
}

void Controller::preferFolder(const std::string& folder) {
  const std::string id = Session::projectIdOf(folder);
  std::vector<KnownProject> all = knownProjects();
  bool found = false;
  for (auto& k : all)
    if (k.id == id) {
      k.folder = normFolder(folder);
      found = true;
    }
  if (!found) {
    KnownProject k;
    k.id = id;
    k.name = Session::projectNameOf(folder);
    k.folder = normFolder(folder);
    all.insert(all.begin(), k);
  }
  writeKnown(all);
}

// Other members' data this computer keeps for a project: their addresses and
// names (<config>/collab/projects/<id>.json) and cached pictures (peers/).
// Emails are never written to disk (Profile.cpp), so there are none to purge.
void Controller::purgePrivate(const std::string& projectId, const std::vector<std::string>& fingerprints) {
  std::error_code ec;
  if (!projectId.empty()) {
    fs::remove(fs::path(profiles_.dir()) / "projects" / (projectId + ".json"), ec);
    // J-146: the project's card (members, their keys) and who was seen working on it
    fs::remove(fs::path(profiles_.dir()) / "projects" / (projectId + ".card.json"), ec);
    fs::remove(fs::path(profiles_.dir()) / "projects" / (projectId + ".seen.json"), ec);
    seen_.erase(projectId);
    if (presence_) presence_->forget(projectId);
  }
  for (const auto& fp : fingerprints)
    if (!fp.empty() && fp.find('/') == std::string::npos && fp != profiles_.identity().fingerprint())
      fs::remove(fs::path(profiles_.dir()) / "peers" / (fp + ".json"), ec);
}

Controller::Controller(std::string collabDir) : profiles_(std::move(collabDir)) { profiles_.load(); }

Controller::~Controller() { close(); }

Session::Config Controller::sessionConfig(const std::string& folder, const std::string& name) const {
  Session::Config c;
  c.folder = folder;
  c.name = name;
  c.identity = profiles_.identity();
  c.profile = profiles_.profile();
  c.profile.color = profiles_.color();
  c.appVersion = version.app;
  return c;
}

void Controller::startHub() {
  std::unique_ptr<Fabric> fabric = fabricFactory ? fabricFactory() : (net.enabled ? makeJuiceFabric(net) : nullptr);
  Profile p = profiles_.profile();
  p.color = profiles_.color();
  hub_ = std::make_unique<Hub>(std::move(fabric), profiles_.identity(), p, version, (fs::path(profiles_.dir()) / "projects").string());
  if (testIceDelaySec > 0) hub_->testIceDelaySec = testIceDelaySec;
  hub_->setSharePresence(showOnline_);  // J-146
  hub_->onNotice = [this](const std::string& t, bool e) {
    if (onNotice) onNotice(t, e);
  };
  hub_->onVersionRefusal = [this](const Hub::Refusal& r) {
    if (onVersionRefusal) onVersionRefusal(r);
  };
  hub_->setSession(session_.get());
  session_->onNotice = [this](const std::string& t, bool e) {
    if (onNotice) onNotice(t, e);
  };
  session_->onRemoteChange = [this](const std::vector<Flash>& f) {
    if (onRemoteChange) onRemoteChange(f);
  };
  session_->onRenamed = [this](const std::string& before, const std::string& after, SiteId by) {
    if (onRenamed) onRenamed(before, after, by);
  };
}

bool Controller::create(Document& doc, const std::string& folder, const std::string& name, std::string* err) {
  if (!profileReady()) {
    if (err) *err = i18n::tr("Choose a display name in Settings > Collaboration first.");
    return false;
  }
  const auto bad = foreignPlugins(songToJson(doc.song(), false));
  if (!bad.empty()) {
    if (err) {
      *err = i18n::tr("This project uses plugins that are not part of Gridline, so it can't be collaborative:");
      for (const auto& [plug, where] : bad) *err += "\n  " + plug + " (" + where + ")";
    }
    return false;
  }
  close();
  session_ = Session::create(doc, sessionConfig(folder, name), err);
  if (session_) stopProbes(session_->projectId());
  if (!session_) return false;
  startHub();
  remember();
  return true;
}

bool Controller::open(Document& doc, const std::string& folder, std::string* err, std::string* needUpdate) {
  const std::string minV = Session::minAppVersionOf(folder);
  if (!minV.empty() && compareVersions(version.app, minV) < 0) {
    if (needUpdate) *needUpdate = minV;
    if (err) *err = i18n::tr("This is a collaborative project. Update Gridline to open it.");
    return false;
  }
  close();
  duplicateOf_.clear();
  exited_ = Exited();
  const Json cj = collabJsonOf(folder);
  const std::string name = cj.text("name");
  // removed / ended: nothing syncs any more; the music opens for keep a private copy / close
  for (const char* key : {"removed", "ended"})
    if (const Json* r = cj.find(key); r && r->is(Json::Type::Object)) {
      Song song;
      if (!loadProject(Session::projectFileOf(folder), song, err)) return false;
      doc.reset(std::move(song));
      exited_ = Exited();
      exited_.kind = std::string(key) == "removed" ? ExitKind::Removed : ExitKind::Ended;
      exited_.projectId = cj.text("id");
      exited_.projectName = name;
      exited_.folder = folder;
      exited_.byName = r->text("byName");
      exited_.reason = r->text("reason");
      exited_.at = static_cast<uint64_t>(r->num("at", 0));
      return true;
    }
  // a copy of a folder this computer already syncs as the same member (copied
  // outside Gridline): both must never sync as one member
  for (const auto& k : knownProjects())
    if (k.id == cj.text("id") && k.state != KnownProject::State::Missing && normFolder(k.folder) != normFolder(folder)) {
      std::error_code ec;
      if (fs::exists(k.folder, ec) && !fs::equivalent(k.folder, folder, ec)) {
        duplicateOf_ = k.folder;
        if (err) *err = i18n::tr("This folder is a copy of a collaborative project that is also at another place on this computer.");
        return false;
      }
    }
  stopProbes(cj.text("id"));  // J-146: its own Hub takes over the ports
  session_ = Session::open(doc, sessionConfig(folder, name), err);
  if (!session_) return false;
  if (session_->replica().visible({"song"}) && !session_->member(session_->site())) {
    // a copy of someone else's project folder (or a reset identity): not a member
    session_.reset();
    if (err) *err = i18n::tr("You are not a member of this collaborative project. Ask a member for an invite.");
    return false;
  }
  if (!session_->ready()) {
    // a join that never received the project
    session_.reset();
    if (err) *err = i18n::tr("This copy never received the project. Ask a member for a new invite and join again (into the same folder).");
    return false;
  }
  startHub();
  remember();
  return true;
}

bool Controller::join(Document& doc, const InviteInfo& info, const std::string& folder, std::string* err) {
  if (!profileReady()) {
    if (err) *err = i18n::tr("Choose a display name in Settings > Collaboration first.");
    return false;
  }
  close();
  stopProbes(info.projectId);  // J-146
  // Re-invited (this computer already has the project): open that copy and
  // connect with the invite; the inviter knows us already, so nobody new is
  // added and nothing is copied again. A copy that never received the project
  // is joined again in place.
  const std::string target = joinTarget(info, folder);
  if (Session::isCollabFolder(target)) {
    if (Session::projectIdOf(target) != info.projectId) {
      if (err) *err = i18n::tr("That folder already holds another collaborative project. Choose another folder.");
      return false;
    }
    std::string openErr;
    std::string name = Session::projectNameOf(target);
    Session::Config cfg = sessionConfig(target, name.empty() ? info.projectName : name);
    // removed before: this invite is an admin's re-invite (the inviter refuses it otherwise)
    Json cj = collabJsonOf(target);
    if (cj.find("removed") || cj.find("ended")) {
      if (cj.find("ended")) {
        if (err) *err = i18n::tr("This project is no longer shared.");
        return false;
      }
      cj.object.erase("removed");
      writeFileDurable((fs::path(target) / ".gridline-collab" / "collab.json").string(), jsonStringify(cj, 2));
      cfg.expectReadmit = true;
    }
    session_ = Session::open(doc, cfg, &openErr);
    if (session_ && (!session_->ready() || !session_->member(session_->site()))) session_.reset();
  }
  if (!session_) session_ = Session::join(doc, sessionConfig(target, info.projectName), info.ticket(), err);
  if (!session_) return false;
  startHub();
  if (!hub_->join(info, err)) {
    close();
    return false;
  }
  remember();
  return true;
}

void Controller::close() {
  if (leave_.active) finishLeave();  // quitting / opening another project: the leave completes now
  writeCardNow();  // J-146: the welcome screen shows it as it was when it closed
  if (hub_) hub_->setSession(nullptr);
  hub_.reset();
  session_.reset();
}

void Controller::frame(double now) {
  now_ = now;
  if (hub_) hub_->poll(now);
  if (session_) session_->frame(now);
  captureExit();
  updateKnownName();  // J-111f
  cachePictures(now);  // J-111g
  cardFrame(now);      // J-146
  presenceFrame(now);
  if (leave_.active) {
    if (leave_.started < 0) {
      leave_.started = now;
      leave_.deadline = now + 6.0;
    }
    // the "left" / "ended" record has reached a collaborator, or nobody is there to tell
    bool anyoneOnline = false;
    if (session_)
      for (const auto& [site, pv] : session_->peers()) anyoneOnline = anyoneOnline || pv.connected;
    if (!session_ || session_->unackedBatches() == 0 || !anyoneOnline || now >= leave_.deadline) finishLeave();
  }
}

void Controller::captureExit() {
  if (!session_ || session_->exitKind() == ExitKind::None || !session_->stopped()) return;
  exited_ = Exited();
  exited_.kind = session_->exitKind();
  exited_.projectId = session_->projectId();
  exited_.projectName = session_->projectName();
  exited_.folder = session_->config().folder;
  if (exited_.kind == ExitKind::Removed) {
    exited_.byName = session_->myRemoval().byName;
    exited_.reason = session_->myRemoval().reason;
    exited_.at = session_->myRemoval().at;
  } else {
    exited_.byName = session_->endInfo().byName;
    exited_.at = session_->endInfo().at;
  }
  std::vector<std::string> fps;
  for (const auto& m : session_->members()) fps.push_back(m.fingerprint());
  const std::string id = session_->projectId();
  close();  // links, invites and port mappings go; the open song stays as it is
  purgePrivate(id, fps);
}

bool Controller::requestLeave(Document& doc, LeaveMode mode, std::string* err) {
  if (!session_ || leave_.active) return false;
  if (session_->isAdmin() && session_->activeMembers().size() > 1) {
    if (err) *err = i18n::tr("You are the admin. Make someone else admin first, or stop sharing.");
    return false;
  }
  if (session_->isAdmin()) return requestStopSharing(doc, err);  // the only member left
  session_->announceLeave();
  leave_ = PendingLeave();
  leave_.active = true;
  leave_.mode = mode;
  leave_.doc = &doc;
  leaveResult_ = LeaveResult();
  return true;
}

bool Controller::requestStopSharing(Document& doc, std::string* err) {
  if (!session_ || leave_.active) return false;
  if (!session_->isAdmin()) {
    if (err) *err = i18n::tr("Only the project admin can stop sharing.");
    return false;
  }
  session_->stopSharing();
  leave_ = PendingLeave();
  leave_.active = true;
  leave_.stop = true;
  leave_.doc = &doc;
  leaveResult_ = LeaveResult();
  return true;
}

bool Controller::convertToNormal(Document& doc, std::string* file, std::string* err) {
  if (!session_) return false;
  if (session_->isAdmin()) session_->stopSharing();
  else session_->announceLeave();
  leave_ = PendingLeave();
  leave_.active = true;
  leave_.stop = session_->isAdmin();
  leave_.doc = &doc;
  finishLeave();
  const LeaveResult r = takeLeaveResult();
  if (file) *file = r.file;
  if (err) *err = r.error;
  return r.error.empty();
}

void Controller::finishLeave() {
  leave_.active = false;
  LeaveResult r;
  r.done = true;
  if (!session_ || !leave_.doc) {
    leaveResult_ = r;
    return;
  }
  const Song song = leave_.doc->song();
  const std::string folder = session_->config().folder, id = session_->projectId();
  const std::string name = session_->config().name.empty() ? session_->projectName() : session_->config().name;
  std::vector<std::string> fps;
  for (const auto& m : session_->members()) fps.push_back(m.fingerprint());
  session_->leave(false);  // goodbye to whoever is online; collab.json says "left"
  if (hub_) hub_->poll(now_);
  close();
  forgetProject(folder);
  purgePrivate(id, fps);
  if (!leave_.stop && leave_.mode == LeaveMode::Delete) {
    std::string e;
    if (moveToTrash(folder, &e)) {
      r.deleted = true;
    } else {
      // nothing deleted: the folder stays, as a normal project
      exportPlainProject(song, folder, folder, name, true, &r.file);
      r.error = e;
    }
  } else if (!exportPlainProject(song, folder, folder, name, true, &r.file, &r.error)) {
    r.file.clear();
  }
  leaveResult_ = r;
}

Controller::LeaveResult Controller::takeLeaveResult() {
  LeaveResult r = leaveResult_;
  leaveResult_ = LeaveResult();
  return r;
}

bool Controller::keepPrivateCopy(Document& doc, const std::string& destFolder, std::string* file, std::string* err) {
  if (!exited()) return false;
  std::error_code ec;
  if (fs::exists(fs::path(destFolder) / ".gridline-collab", ec)) {
    if (err) *err = i18n::tr("That folder holds a collaborative project. Choose another one.");
    return false;
  }
  const std::string name = exited_.projectName.empty() ? std::string("Project") : exited_.projectName;
  if (!exportPlainProject(doc.song(), exited_.folder, destFolder, name, false, file, err)) return false;
  return true;
}

// J-111g: collaborators' pictures are kept in <config>/collab/peers (by signing-key
// fingerprint) once they arrived, and a picture already kept is not fetched again
// (reopening the project, reconnecting, the People list while they are offline).
void Controller::cachePictures(double now) {
  if (!session_ || (picturesAt_ >= 0 && now - picturesAt_ < 1.0 && now >= picturesAt_)) return;  // (once a second)
  picturesAt_ = now;
  for (const auto& [site, pv] : session_->peers()) {
    if (pv.fingerprint.empty() || pv.profile.name.empty()) continue;
    const Profile* known = profiles_.known(pv.fingerprint);
    const std::string h = pv.profile.avatarHash();
    if (!pv.profile.avatar.empty() || h.empty()) {  // what they show now (a new picture, or none)
      if (!known || known->avatarHash() != h || known->name != pv.profile.name || known->color != pv.profile.color)
        if (pv.profile.avatar.empty() || session_->avatarFor(h) == pv.profile.avatar) profiles_.remember(pv.fingerprint, pv.profile);
    } else if (known && !known->avatar.empty() && known->avatarHash() == h && session_->avatarFor(h).empty()) {
      session_->addAvatar(known->avatar);  // the bytes are on this computer already
    }
  }
}

// ---- J-111f: the project's name ----
void Controller::updateKnownName() {
  if (!session_ || !session_->ready() || session_->stopped()) return;
  const std::string n = session_->projectName();
  if (n == knownName_) return;
  knownName_ = n;
  remember();  // Recent collaborative projects shows the new name
}

bool Controller::rename(const std::string& name, std::string* err) {
  if (!session_) {
    if (err) *err = i18n::tr("No collaborative project is open.");
    return false;
  }
  return session_->rename(name, err);
}

std::string Controller::folderForName() const {
  if (!session_) return {};
  const fs::path cur = fs::path(session_->config().folder).lexically_normal();
  const fs::path target = cur.parent_path() / projectFileStem(session_->projectName());
  return target.filename() == cur.filename() ? std::string() : target.string();
}

bool Controller::renameFolder(Document& doc, std::string* folderOut, std::string* err) {
  if (!session_ || !session_->ready()) {
    if (err) *err = i18n::tr("No collaborative project is open.");
    return false;
  }
  const std::string from = fs::path(session_->config().folder).lexically_normal().string();
  const std::string to = folderForName();
  if (folderOut) *folderOut = from;
  if (to.empty()) return true;
  std::error_code ec;
  const bool sameFolder = fs::exists(to, ec) && fs::equivalent(from, to, ec);  // only the case differs
  if (fs::exists(to, ec) && !sameFolder) {
    if (err) *err = i18n::tr("A folder with that name is already there. Rename or move it first.");
    return false;
  }
  close();  // the log, the project file and the connections are closed first
  if (sameFolder) {
    const std::string tmp = from + ".gridline-rename";
    fs::rename(from, tmp, ec);
    if (!ec) {
      fs::rename(tmp, to, ec);
      if (ec) {
        std::error_code back;
        fs::rename(tmp, from, back);
      }
    }
  } else {
    fs::rename(from, to, ec);
  }
  const bool moved = !ec;
  const std::string reason = ec.message();
  std::string openErr;
  if (!open(doc, moved ? to : from, &openErr)) {
    if (err) *err = openErr;
    return false;
  }
  if (!moved) {
    if (err) *err = i18n::tr("The folder could not be renamed (is a file in it open in another program?)") + std::string(": ") + reason;
    return false;
  }
  // (open() remembered the new folder; the old entry of this project id went with it)
  if (folderOut) *folderOut = to;
  return true;
}

void Controller::setProfile(const Profile& p) {
  profiles_.profile() = p;
  profiles_.save();
  Profile wire = p;
  wire.color = profiles_.color();
  if (session_) session_->setProfile(wire);
  if (hub_) hub_->setProfile(wire);
}

}  // namespace gl::collab

// ---- J-146: the welcome screen's Collaborative view ----
namespace gl::collab {

void Controller::setShowOnline(bool on) {
  if (showOnline_ == on && (!presence_ || presence_->enabled() == on)) return;
  showOnline_ = on;
  if (hub_) hub_->setSharePresence(on);
  if (presence_) presence_->setEnabled(on);
}

PresenceProber& Controller::presence() {
  if (!presence_) {
    std::unique_ptr<Fabric> fabric = fabricFactory ? fabricFactory() : (net.enabled ? makeJuiceFabric(net) : nullptr);
    Profile p = profiles_.profile();
    p.color = profiles_.color();
    presence_ = std::make_unique<PresenceProber>(std::move(fabric), profiles_.identity(), p, version);
    presence_->setEnabled(showOnline_);
  }
  return *presence_;
}

void Controller::stopProbes(const std::string& projectId) {
  if (presence_ && !projectId.empty()) presence_->forget(projectId);
}

void Controller::writeCardNow() {
  if (!session_ || !session_->ready() || session_->stopped() || session_->projectId().empty()) return;
  const ProjectCard c = cardOf(*session_, session_->wallNowMs());
  Json j = cardToJson(c);
  j.object.erase("saved");
  std::string text = jsonCompact(j);
  // members connected now were seen working on it now
  const int64_t nowSec = static_cast<int64_t>(session_->wallNowMs() / 1000);
  auto& seen = seen_[c.id];
  if (seen.empty()) seen = readSeen(profiles_.dir(), c.id);
  bool seenChanged = false;
  for (const auto& [site, pv] : session_->peers())
    if (pv.connected && (seen[site] == 0 || nowSec - seen[site] >= 30)) {
      seen[site] = nowSec;
      seenChanged = true;
    }
  if (seenChanged) writeSeen(profiles_.dir(), c.id, seen);
  if (text == cardText_) return;
  cardText_ = text;
  writeCard(profiles_.dir(), c);
}

void Controller::cardFrame(double now) {
  if (!session_ || (cardAt_ >= 0 && now - cardAt_ < 3.0 && now >= cardAt_)) return;
  cardAt_ = now;
  writeCardNow();
}

void Controller::presenceFrame(double now) {
  if (!presence_) return;
  presence_->poll(now);  // (does nothing while the welcome screen's view is not shown)
  if (presence_->takeChanged()) {
    // fresh "working now" answers update the last-seen cache
    for (const auto& k : knownProjects()) {
      const auto* rs = presence_->results(k.id);
      if (!rs) continue;
      auto& seen = seen_[k.id];
      if (seen.empty()) seen = readSeen(profiles_.dir(), k.id);
      bool changed = false;
      for (const auto& [site, r] : *rs)
        if (r.state == PresenceState::Working && r.checkedAt > 0 && r.checkedAt - seen[site] >= 30) {
          seen[site] = r.checkedAt;
          changed = true;
        }
      if (changed) writeSeen(profiles_.dir(), k.id, seen);
    }
  }
}

std::map<std::string, ProjectCard> Controller::cards() const {
  std::map<std::string, ProjectCard> out;
  for (const auto& k : knownProjects()) {
    ProjectCard c;
    if (readCard(profiles_.dir(), k.id, c)) out[k.id] = std::move(c);
  }
  return out;
}

std::vector<ListProject> Controller::listProjects() const {
  std::vector<ListProject> out;
  for (const auto& k : knownProjects()) {
    ListProject l;
    l.id = k.id;
    l.name = k.name;
    l.folder = k.folder;
    l.openedMs = k.openedMs;
    l.state = k.state == KnownProject::State::Removed   ? ListProject::State::Removed
              : k.state == KnownProject::State::Ended   ? ListProject::State::Ended
              : k.state == KnownProject::State::Missing ? ListProject::State::Missing
                                                        : ListProject::State::Active;
    l.byName = k.byName;
    l.reason = k.reason;
    l.at = k.at;
    out.push_back(std::move(l));
  }
  return out;
}

std::map<std::string, std::map<SiteId, int64_t>> Controller::seen() const {
  std::map<std::string, std::map<SiteId, int64_t>> out;
  for (const auto& k : knownProjects()) {
    auto it = seen_.find(k.id);
    out[k.id] = it != seen_.end() && !it->second.empty() ? it->second : readSeen(profiles_.dir(), k.id);
  }
  return out;
}

}  // namespace gl::collab

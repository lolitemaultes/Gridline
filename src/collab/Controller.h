// Gridline - J-111 collaboration: what the app (and the command line) holds
// while a collaborative project is open: the user's profile store, the
// session and its network hub. Starting, opening, joining and leaving go
// through here so the plugin policy, the version record and the profile
// requirements are enforced in one place.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../core/Document.h"
#include "Hub.h"
#include "Presence.h"
#include "ProjectList.h"
#include "Profile.h"
#include "Session.h"

namespace gl {
class Settings;
}

namespace gl::collab {

// The network settings from the app settings (TURN password: secret store).
NetSettings netSettingsFrom(const Settings& s);
// This build's version, protocol and build id.
VersionInfo currentVersion();

class Controller {
 public:
  explicit Controller(std::string collabDir);  // <config>/gridline/collab
  ~Controller();

  ProfileStore& profiles() { return profiles_; }
  bool profileReady() const { return profiles_.ready(); }
  NetSettings net;
  VersionInfo version = currentVersion();
  // A fabric for tests (in-memory); default: libjuice with `net`.
  std::function<std::unique_ptr<Fabric>()> fabricFactory;
  double testIceDelaySec = 0;  // tests: see Hub::testIceDelaySec

  bool active() const { return session_ != nullptr; }
  Session* session() { return session_.get(); }
  Hub* hub() { return hub_.get(); }

  // Refusal reasons (UI): these return false and set `err`; `needUpdate` is
  // set when the project needs a newer Gridline (minAppVersion).
  // A collaborative project on this computer (create, open, join: newest first).
  struct KnownProject {
    std::string id, name, folder;
    uint64_t openedMs = 0;
    // Removed: an admin removed you; Ended: the admin stopped sharing; Missing:
    // the folder was moved, renamed or deleted outside Gridline (Locate / Forget).
    enum class State { Active, Removed, Ended, Missing } state = State::Active;
    std::string byName, reason;
    uint64_t at = 0;
  };
  std::vector<KnownProject> knownProjects() const;  // every remembered project, missing folders too
  // The project moved: `folder` holds it now (its id decides).
  bool locateProject(const std::string& projectId, const std::string& folder, std::string* err = nullptr);
  std::string knownFolder(const std::string& projectId) const;
  // Where joining with this invite puts the project: the copy this computer
  // already has (re-invited: it reconnects) or `folder`.
  std::string joinTarget(const InviteInfo& info, const std::string& folder) const;
  // Off the list. A removed / ended folder also loses its shared history (it stays a normal project).
  void forgetProject(const std::string& folder);

  bool create(Document& doc, const std::string& folder, const std::string& name, std::string* err);
  bool open(Document& doc, const std::string& folder, std::string* err, std::string* needUpdate = nullptr);
  bool join(Document& doc, const InviteInfo& info, const std::string& folder, std::string* err);
  void close();                        // the session stops; the folder keeps everything
  void frame(double now);

  // ---- the end of collaboration for this computer (J-111e; docs: "What happens to the files") ----
  // Leave (anyone but the admin of others): announces "left" (signed), waits a moment for it
  // to reach a collaborator, then keeps the folder as a normal project or moves it to the trash.
  enum class LeaveMode { Keep, Delete };
  bool requestLeave(Document& doc, LeaveMode mode, std::string* err = nullptr);
  // Admin: stop sharing for everyone; this copy becomes a normal project.
  bool requestStopSharing(Document& doc, std::string* err = nullptr);
  // Nobody came back (abandoned): this copy becomes a normal project now.
  bool convertToNormal(Document& doc, std::string* file, std::string* err = nullptr);
  bool leaving() const { return leave_.active; }
  // The result of the last leave / stop sharing: done, the project file kept ("" when deleted), error.
  struct LeaveResult {
    bool done = false, deleted = false;
    std::string file, error;
  };
  LeaveResult takeLeaveResult();
  // The open project stopped syncing by itself (an admin removed you, or stopped
  // sharing), or such a folder was opened: what happened, for the dialog.
  struct Exited {
    ExitKind kind = ExitKind::None;
    std::string projectId, projectName, folder, byName, reason;
    uint64_t at = 0;
  };
  const Exited* exited() const { return exited_.kind == ExitKind::None ? nullptr : &exited_; }
  void dismissExit() { exited_ = Exited(); }
  // A normal project from the open (exited) project in `destFolder`: the music and
  // its audio, no collaboration data. `file` receives the project file.
  bool keepPrivateCopy(Document& doc, const std::string& destFolder, std::string* file, std::string* err = nullptr);
  // Open refused because the folder is a copy of a project this computer has
  // elsewhere (both would sync as the same member): that other folder.
  const std::string& duplicateOf() const { return duplicateOf_; }
  // "Use this folder instead": the known copy becomes this one.
  void preferFolder(const std::string& folder);
  void setProfile(const Profile& p);   // saved + sent to collaborators
  // J-111f: renames the open project for everyone (Session::rename).
  bool rename(const std::string& name, std::string* err);
  // J-111f: this computer's project folder takes the project's name (an
  // explicit choice, never automatic): the session closes, the folder is
  // renamed (another folder of that name is never replaced) and the project
  // reopens from it and reconnects. `folder` becomes the folder in use.
  bool renameFolder(Document& doc, std::string* folder, std::string* err);
  // Where renameFolder() would move the folder ("" when it already has the name).
  std::string folderForName() const;

  // ---- J-146: the welcome screen's Collaborative view ----
  const std::string& collabDir() const { return profiles_.dir(); }
  // "Show that I am online to collaborators" (settings: collab.showOnline): the open
  // project's Hub answers presence probes, and this computer probes others, only when on.
  void setShowOnline(bool on);
  bool showOnline() const { return showOnline_; }
  // The presence prober (made on first use; its own sockets, never the open project's).
  PresenceProber& presence();
  bool hasPresence() const { return presence_ != nullptr; }
  // The open project's card, written now (also every few seconds while it is open and on close).
  void writeCardNow();
  // The cards of the known projects (from disk, cached by file time).
  std::map<std::string, ProjectCard> cards() const;
  // "Last seen working on it" per project (probes and sessions).
  std::map<std::string, std::map<SiteId, int64_t>> seen() const;
  // The known projects as the list model takes them (ProjectList.h).
  std::vector<ListProject> listProjects() const;

  std::function<void(const std::string&, bool)> onNotice;
  std::function<void(const Hub::Refusal&)> onVersionRefusal;
  std::function<void(const std::vector<Flash>&)> onRemoteChange;
  std::function<void(const std::string& before, const std::string& after, SiteId by)> onRenamed;  // J-111f

 private:
  Session::Config sessionConfig(const std::string& folder, const std::string& name) const;
  void remember();  // the open session's project, as the newest
  void writeKnown(const std::vector<KnownProject>& all) const;
  void purgePrivate(const std::string& projectId, const std::vector<std::string>& fingerprints);
  void captureExit();
  void finishLeave();
  struct PendingLeave {
    bool active = false, stop = false;
    LeaveMode mode = LeaveMode::Keep;
    Document* doc = nullptr;
    double deadline = 0, started = -1;
  } leave_;
  LeaveResult leaveResult_;
  Exited exited_;
  std::string duplicateOf_;
  double now_ = 0;
  void startHub();
  // J-111f hook (known projects, J-111e's area): the open project's entry follows its shared name.
  void updateKnownName();
  void cachePictures(double now);  // J-111g: collaborators' pictures kept on this computer
  double picturesAt_ = -1;  std::string knownName_;
  ProfileStore profiles_;
  std::unique_ptr<Session> session_;
  std::unique_ptr<Hub> hub_;
  // J-146
  void cardFrame(double now);
  void presenceFrame(double now);
  void stopProbes(const std::string& projectId);
  std::unique_ptr<PresenceProber> presence_;
  bool showOnline_ = true;
  double cardAt_ = -1;
  std::string cardText_;
  std::map<std::string, std::map<SiteId, int64_t>> seen_;  // loaded per project on use
};

}  // namespace gl::collab

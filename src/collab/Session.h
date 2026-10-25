// Gridline - J-111 collaboration: a collaborative project session.
//
// Glues the open Document to the replicated document (Crdt.h): local changes
// are diffed into signed op batches (30 Hz while a gesture merges, at once on
// a new step), remote batches are applied in causal order and materialised
// into the Document once per frame, undo / redo undo only your own ops, the op
// log on disk is the project's continuous save, audio files are content
// addressed in <folder>/Audio and fetched from peers, conflicts of offline
// edits are detected and queued for the admin. No network code here: the
// transport (Net.h) calls receive() / peerUp() / peerDown() and is called
// through sendFn, so tests can wire sessions together in memory.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../core/Document.h"
#include "Conflicts.h"
#include "Crdt.h"
#include "Crypto.h"
#include "Log.h"
#include "Profile.h"

namespace gl::collab {

inline constexpr uint32_t kIdStride = 64;          // id partition (max 63 collaborators + spare)
inline constexpr double kGestureFlushSec = 1.0 / 30.0;
inline constexpr double kPresenceSec = 1.0 / 20.0;
inline constexpr double kAvatarRetrySec = 3.0;  // J-111g: a picture asked for and not here yet is asked for again
inline constexpr double kLogSyncSec = 0.25;
inline constexpr double kProjectWriteSec = 2.0;
inline constexpr uint64_t kCompactBytes = 4u << 20;
inline constexpr size_t kCompactRecords = 20000;
inline constexpr size_t kFileChunk = 16 * 1024;

// ---- the project's name (J-111f) ----
// The display name is shared data: meta/project/name in the replica, a
// last-writer-wins register any member may set (docs: "Naming and renaming").
// Each computer's project file is named after it (sanitised), the folder is not.
inline constexpr size_t kMaxProjectNameChars = 80;
inline constexpr const char* kProjectFileExt = ".gridline.json";
// A display name: trimmed, control and invisible formatting characters
// removed, runs of spaces collapsed, at most kMaxProjectNameChars characters
// (UTF-8 aware; any script). "" when nothing is left.
std::string cleanProjectName(const std::string& raw);
// True when `name` is a valid display name exactly as written (cleanProjectName(name) == name, not empty).
bool validProjectName(const std::string& name);
// A file / folder name for a display name that is safe on Linux, macOS and
// Windows: no / \ : * ? " < > |, no leading dots, no trailing dots or spaces,
// not a reserved Windows device name (CON, NUL, COM1...), at most 120 bytes
// (cut on a character boundary). Never empty ("Project").
std::string projectFileStem(const std::string& name);

struct Member {
  SiteId site = 0;
  crypto::Key signPublic{};
  crypto::Key dhPublic{};
  int slot = 0;
  std::string name;   // name snapshot when added
  SiteId by = 0;      // who invited
  uint64_t joinedMs = 0;
  std::string fingerprint() const { return crypto::fingerprintOf(signPublic); }
};

// Where a collaborator's mouse is (see docs: presence).
// J-111d: `kind`, `a`, `b`, `cx`, `cy` are an anchor (app/CursorAnchor.h): what the
// pointer is over (an editor surface in content coordinates, an interface item), so
// it lands on the same thing on a screen of another size, scale, scroll or zoom;
// x, y stay the fraction of the window (kind 0, and older peers).
struct Presence {
  std::string window;    // window key ("" = not over a Gridline window)
  float x = 0, y = 0;    // normalised in the window
  int kind = 0;          // anchor kind (0: the window fraction only)
  uint32_t a = 0, b = 0; // anchor ids (item ID, track index, pattern / channel, mixer track)
  float cx = 0, cy = 0;  // anchor coordinates (ticks, rows, fractions; see CursorAnchor.h)
  // J-111g: the plugin whose editor the pointer is in (Gridline's own editor windows,
  // an installed plugin's window): the channel or mixer track (shared object id;
  // 0: not in a plugin editor) and the effect slot (-1: the channel's instrument).
  uint32_t plugin = 0;
  int pluginSlot = -1;
  bool playing = false;
  double songTick = 0;   // transport position (ticks)
  bool operator==(const Presence& o) const {
    return window == o.window && x == o.x && y == o.y && kind == o.kind && a == o.a && b == o.b && cx == o.cx && cy == o.cy &&
           plugin == o.plugin && pluginSlot == o.pluginSlot && playing == o.playing && songTick == o.songTick;
  }
};

struct PeerView {
  SiteId site = 0;
  Profile profile;           // name, colour, avatar (email only when shared)
  std::string fingerprint;
  bool connected = false;
  double rttMs = -1;
  uint64_t lastSyncMs = 0;   // wall ms of the last exchange
  size_t unseen = 0;         // our batches they have not seen (from their last version vector)
  size_t syncTotal = 0, syncDone = 0;  // catch-up progress (batches)
  Presence presence;
  double presenceAt = -1;    // session time of the last presence message
  bool clockApprox = false;  // their clock disagrees with ours by more than 5 s
  VersionVector known;       // their last version vector, as they reported it (stability, compaction)
  VersionVector seen;        // what they have at least (reported, or sent over the reliable link)
  double avatarAskedAt = -1; // J-111g: when their picture's bytes were last asked for
};

// J-111e membership (signed, replicated; docs: "Removing someone", "What happens to the files").
enum class MemberState { Unknown, Active, Removed, Left };

// One removal of a member by the admin (meta/removed/<site>/<id>), with its re-add if any.
struct Removal {
  std::string id;            // record id (admin site - stamp)
  SiteId site = 0;           // who was removed
  SiteId by = 0;             // the admin who removed them
  std::string name, byName;  // names when it happened (history stays readable)
  std::string reason;        // optional text shown to them
  uint64_t at = 0;           // wall ms
  uint64_t upTo = 0;         // their batches up to this seq stay; later ones are dropped everywhere
  bool readded = false;      // re-invited by an admin afterwards
  SiteId readdBy = 0;
  uint64_t readdSeq = 0;     // the admin's batch that re-added them (their later batches depend on it)
  uint64_t readdAt = 0;
};

// Stop sharing (meta/ended): the admin ended the project for everyone.
struct EndInfo {
  SiteId by = 0;
  std::string byName;
  uint64_t at = 0;
};

// Why a session stopped on its own (the UI offers keep a private copy / close).
enum class ExitKind { None, Removed, Ended };

struct FileProgress {
  std::string name;          // "Audio/<sha256>.<ext>"
  uint64_t have = 0, total = 0;
  bool failed = false;
};

struct Flash {
  std::string object;        // "channels/#5", "playlist/#40", "mixer/#3", "patterns/#2"
  SiteId site = 0;
  double at = 0;
};

class Session {
 public:
  struct Config {
    std::string folder;        // the collaborative project folder
    std::string name;          // create: the project's name; join: the invite's (until the first sync); open: a fallback
    crypto::Identity identity;
    Profile profile;
    std::string appVersion;    // running Gridline version
    bool durable = true;       // fsync the log (tests may turn it off)
    std::function<uint64_t()> wallMs;  // clock (tests); default system clock
    // Joining again after being removed (an admin's re-invite): our own removal
    // does not stop the session while the re-add record is on its way.
    bool expectReadmit = false;
  };
  // What an invitee knows before the first sync (from the invite code).
  struct Ticket {
    std::string projectId;
    std::string projectName;
    SiteId creator = 0;
    crypto::Key creatorSign{};
  };

  // A new collaborative project from the document's current song (the
  // caller checked the plugin policy). Collects the files into <folder>/Audio.
  static std::unique_ptr<Session> create(Document& doc, Config cfg, std::string* err = nullptr);
  // Opens an existing collaborative project folder.
  static std::unique_ptr<Session> open(Document& doc, Config cfg, std::string* err = nullptr);
  // An empty project waiting for its first sync from the inviter.
  static std::unique_ptr<Session> join(Document& doc, Config cfg, const Ticket& t, std::string* err = nullptr);
  // True when `folder` holds a collaborative project.
  static bool isCollabFolder(const std::string& folder);
  // The collaborative project folder of a project file, or "".
  static std::string folderOfProjectFile(const std::string& projectFile);
  // minAppVersion recorded in a collaborative project folder ("" when unknown).
  static std::string minAppVersionOf(const std::string& folder);
  // From the folder's .gridline-collab/collab.json ("" when it is not one).
  static std::string projectIdOf(const std::string& folder);
  // The display name as last written to the folder (collab.json), "" when unknown.
  static std::string projectNameOf(const std::string& folder);
  // The project file of a collaborative project folder, found without
  // trusting its name: collab.json's "file" record, else a file named after
  // the project, else the only / the last written *.gridline.json in the
  // folder (a rename interrupted by a crash, a file renamed by hand). When the
  // folder has none yet: where it will be written.
  static std::string projectFileOf(const std::string& folder);
  // Renames <folder>/<from> to the first free "<stem>.gridline.json",
  // "<stem> (2).gridline.json" ... (a file of another name is never replaced;
  // a case-only rename goes through a temporary name, for case-insensitive
  // file systems). `from` may not exist yet. Returns the file name now in
  // use (`from` when the rename failed, `err` set).
  static std::string renameProjectFileIn(const std::string& folder, const std::string& from, const std::string& stem,
                                         std::string* err = nullptr);

  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // ---- per frame (UI thread) ----
  void frame(double now);
  // Diffs local changes now (called before undo/redo and new steps; frame()
  // throttles it to 30 Hz while a gesture merges).
  void flushLocal();
  // The user is in the middle of a mouse gesture (drag): remote ops on the
  // objects it edits wait until it ends.
  void setGesture(bool active) { gesture_ = active; }
  void setPresence(const Presence& p) { localPresence_ = p; }

  // ---- transport ----
  std::function<void(SiteId to, int stream, const std::string& wire)> sendFn;
  void peerUp(SiteId peer);
  void peerDown(SiteId peer);
  void receive(SiteId from, int stream, const std::string& wire);
  void setRtt(SiteId peer, double ms);
  // Bytes waiting in the bulk stream to `peer` (file sending is paced by it).
  std::function<size_t(SiteId peer)> bulkQueuedFn;
  // File bytes sent per frame at most (the transport paces further).
  size_t fileBudget = 2u << 20;
  // An invitee completed the handshake: records them as a member (signed op).
  // Returns the slot assigned.
  int admitMember(const Member& m);

  // ---- notifications (UI) ----
  std::function<void(const std::string& text, bool error)> onNotice;
  std::function<void(const std::vector<Flash>&)> onRemoteChange;
  // The shared name changed (a collaborator's rename, yours, an undo of one, a
  // sync review choice): `by` wrote the name now in place. Not called for the
  // name a join receives with the project.
  std::function<void(const std::string& before, const std::string& after, SiteId by)> onRenamed;

  // ---- queries ----
  const Config& config() const { return cfg_; }
  SiteId site() const { return me_; }
  const std::string& projectId() const { return projectId_; }
  // The shared name (meta/project/name), else the name from collab.json /
  // the invite, else the project file's name.
  std::string projectName() const;
  std::string projectFile() const;   // <folder>/<file named after the project>
  bool ready() const { return ready_; }   // the replica holds a project (joiners: after the first sync)
  bool isAdmin() const { return admin() == me_; }
  SiteId admin() const;
  SiteId creator() const { return creator_; }
  std::vector<Member> members() const;
  const Member* member(SiteId s) const;
  const std::map<SiteId, PeerView>& peers() const { return peers_; }
  PeerView* peer(SiteId s);
  const VersionVector& versionVector() const { return vv_; }
  std::string minAppVersion() const;
  std::vector<FileProgress> transfers() const;
  // A referenced file of this project is still arriving (placeholder state);
  // `progress` 0..1.
  bool fileLoading(const std::string& absPath, float* progress = nullptr) const;
  size_t pendingBatches() const;
  uint64_t opCount() const { return opCount_; }
  Replica& replica() { return replica_; }
  const Replica& replica() const { return replica_; }
  OpLog& log() { return log_; }
  // Last flush / apply costs (performance budgets), milliseconds.
  double lastFlushMs() const { return lastFlushMs_; }
  double lastApplyMs() const { return lastApplyMs_; }
  // How remote edits were applied: element by element / whole project, and how
  // often the quiet-time whole-project check had to correct the former.
  uint64_t incrementalApplies() const { return incApplies_; }
  uint64_t fullApplies() const { return fullApplies_; }
  uint64_t corrections() const { return corrections_; }
  // Wall ms of the newest op from each author (logs).
  uint64_t wallNowMs() const;

  // ---- membership (J-111e) ----
  MemberState memberState(SiteId s) const;
  bool isActive(SiteId s) const { return memberState(s) == MemberState::Active; }
  // Members who have not left and are not removed (members() lists everyone ever admitted).
  std::vector<Member> activeMembers() const;
  // Every removal (audit trail), oldest first; the open one of `s` (nullptr when none).
  std::vector<Removal> removals() const;
  const Removal* openRemoval(SiteId s) const;
  // Admin only: removes `s` (not yourself: use Leave). A signed op every peer applies;
  // their later batches are dropped everywhere and every peer refuses them.
  bool removeMember(SiteId s, const std::string& reason, std::string* err = nullptr);
  // Admin only, from a re-invite: clears the removed state of `s`.
  bool readmit(SiteId s);
  // Why we stopped (removed by the admin, or the admin stopped sharing) and the details.
  ExitKind exitKind() const { return exit_; }
  const Removal& myRemoval() const { return myRemoval_; }
  const EndInfo& endInfo() const { return end_; }
  bool ended() const { return ended_; }
  // What a refused peer is told (signed proof when we still hold it), Null when they are welcome.
  Json noticeFor(SiteId peer) const;
  // A refusal told us we were removed / the project ended (offline catch-up). False when it does not hold.
  bool learnNotice(const Json& notice);
  // Our batches no collaborator has confirmed yet (Leave warns about them).
  size_t unackedBatches() const;
  // Leave: announces "left" (a signed op) before the session stops.
  bool announceLeave();
  // Admin: stop sharing for everyone (a signed "ended" op).
  bool stopSharing();
  // Wall ms of the last exchange with any collaborator (abandoned projects), 0 = never.
  uint64_t lastContactMs() const { return lastContact_; }
  // Our replica held edits a removal cut off and the snapshot on disk includes them:
  // waiting for a clean snapshot from a collaborator.
  bool tainted() const { return tainted_; }
  uint64_t rebuilds() const { return rebuilds_; }
  // Audio files in <folder>/Audio nothing refers to any more (not the project, the
  // history we hold, undo / redo or the Sync review): name and size.
  std::vector<std::pair<std::string, uint64_t>> unusedAudio() const;
  // Deletes those files; returns the bytes freed.
  uint64_t cleanUnusedAudio();

  // ---- conflicts (admin review) ----
  ConflictTracker& conflicts() { return conflicts_; }
  const ConflictTracker& conflicts() const { return conflicts_; }
  // Admin only: keeps `keepSite`'s side of `object` (a signed resolution + the
  // ops making that side current everywhere). False when not admin.
  bool resolve(const std::string& object, SiteId keepSite, std::string* err = nullptr);
  // Applies a side locally only (audition); endPreview() goes back.
  bool preview(const std::string& object, SiteId keepSite);
  void endPreview();
  bool previewing() const { return previewing_; }
  bool transferAdmin(SiteId to);
  // True once after a sync produced conflicts the admin has not seen (UI opens the review).
  bool takeReviewRequest();

  // ---- name ----
  // Renames the project for everyone: a signed last-writer-wins write of
  // meta/project/name, one undo step of its own. The same name (after
  // cleaning) is accepted and changes nothing. False with `err` for an empty
  // name, before the project arrived, or after leaving.
  bool rename(const std::string& name, std::string* err = nullptr);

  // ---- profile ----
  void setProfile(const Profile& p);
  // Avatar bytes received for a hash ("" when not here).
  std::string avatarFor(const std::string& hash) const;
  // J-111g: bytes known from elsewhere (the picture cache of people met before):
  // kept by their hash, so a picture already here is not asked for again.
  void addAvatar(const std::string& bytes);

  // Stops syncing: the folder stays a plain project. `stopSharing`: also tells
  // everyone (creator) that the project is no longer shared.
  void leave(bool stopSharing);
  bool stopped() const { return stopped_; }
  // Writes the log, the snapshot if due, and the project file now.
  void saveNow();
  // Forces a compaction (tests / maintenance).
  bool compactNow(std::string* err = nullptr);

  // Paths: "Audio/<file>" <-> <folder>/Audio/<file>.
  std::string localPath(const std::string& rel) const;

 private:
  Session(Document& doc, Config cfg);
  void attach();
  void detach();
  bool loadFromDisk(std::string* err);
  bool bootstrap(std::string* err);  // create(): genesis batch
  // local changes
  bool makeLocalBatch(std::vector<Op> ops, bool undoable, bool recordStep);
  void recordStep(const std::vector<Op>& ops, const std::vector<Json>& before);
  bool historyStep(int dir);
  Json localSongJson();
  void refsToReplica(Json& song);
  void refsToLocal(Json& song) const;
  bool policyAllows(const Json& song, std::string* offending) const;
  // remote
  void handleBatches(SiteId from, const Json& list);
  bool tryApply(const Batch& b, bool local);
  void drainPending();
  bool authorized(const Batch& b, std::string* why) const;
  bool honoured(const Batch& b) const;
  bool metaAllowed(const Batch& b, const Op& op, SiteId adminNow) const;
  void refreshMembership();
  void rebuild();
  void requestCleanSnapshot();
  void finishExit();
  std::string batchHolding(SiteId author, const Path& path) const;
  void writeCollabFlag(const char* key, const Json& value);
  bool heldByGesture(const Batch& b) const;
  void materialize();
  bool diffIncremental(std::vector<Op>& ops, std::string* bad);
  bool applyIncremental();
  bool buildFullSong(Song& s, std::string* err = nullptr);
  void verifyNow();
  void sendHello(SiteId to);
  void sendMissing(SiteId to, const VersionVector& theirs);
  void sendSnapshot(SiteId to);
  void applySnapshot(const Message& m);
  void broadcastBatch(const std::string& text, SiteId except);
  Json snapshotJson() const;
  void maybeCompact();
  void writeProjectFile();
  void writeCollabJson();       // collab.json: the name, the file, the newest version that wrote here
  void checkName();             // the shared name changed: rename the file, tell the UI
  void syncFileName();          // the project file follows the shared name
  bool isStable(const Batch& b) const;
  void updateReady();
  // files
  void scanFiles();
  void requestFiles();
  void onFileWant(SiteId from, const Message& m);
  void onFileData(SiteId from, const Message& m);
  void pumpFileSends();
  void finishFile(const std::string& name);
  std::string importFile(const std::string& absPath);
  // presence / profile
  void sendPresence(double now);
  void askAvatar(SiteId to, PeerView& pv);  // J-111g
  void send(SiteId to, int stream, const Message& m);

  Document& doc_;
  Config cfg_;
  SiteId me_ = 0;
  std::string projectId_;
  SiteId creator_ = 0;
  crypto::Key creatorSign_{};
  std::string fileName_;      // this computer's project file in the folder ("<stem>.gridline.json")
  std::string shownName_;     // the name the UI was last told about
  bool nameReady_ = false;    // ready_ when shownName_ was set (a join's first name is not a rename)
  double fileRetryAt_ = -1;   // a failed file rename (the file is open elsewhere) is retried then
  Replica replica_;
  Hlc hlc_;
  OpLog log_;
  VersionVector vv_;
  uint64_t mySeq_ = 0;
  uint64_t opCount_ = 0;
  // every batch we hold (for catch-up / relay), by author then seq, as signed text
  std::map<SiteId, std::map<uint64_t, std::string>> batches_;
  std::map<SiteId, std::map<uint64_t, Batch>> pending_;
  std::map<SiteId, PeerView> peers_;
  ConflictTracker conflicts_;
  bool ready_ = false, stopped_ = false, attached_ = false, replaying_ = false;
  bool remoteDirty_ = false, floDirty_ = true, metaDirty_ = true;
  std::vector<Flash> flashes_;
  // local diff state
  uint64_t lastRev_ = ~0ull, lastPush_ = 0, lastSerial_ = 0;
  double now_ = 0, lastFlush_ = -1, lastPresence_ = -1, lastLogSync_ = 0, lastVvSend_ = 0;
  double projectDirtyAt_ = -1;
  bool gesture_ = false, wasGesture_ = false;
  std::set<std::string> gestureObjects_;
  FlOrigin floShadow_;
  // the replica as a song (without flOrigin): local changes are found by comparing with it
  Song shadow_;
  bool shadowValid_ = false;
  struct Dirty {
    bool full = false;
    std::map<std::string, std::set<std::string>> elems;  // list -> element ids changed by collaborators
    std::set<std::string> scalars;
  } dirty_;
  double verifyAt_ = -1;
  uint64_t incApplies_ = 0, fullApplies_ = 0, corrections_ = 0;
  FlOrigin floCache_;
  bool reviewRequest_ = false;
  bool previewing_ = false;
  Presence localPresence_, sentPresence_;
  // undo records by history state serial
  struct UndoGroup {
    Path path;
    int cls = 0;  // 0 value, 1 position, 2 presence
    Json backVal, fwdVal;
    std::string backPos, fwdPos;
    bool backPres = false, fwdPres = false;
    Json backContent;  // presence: content to re-insert when the node is gone
    Stamp cur;
  };
  struct StepRec {
    std::vector<UndoGroup> groups;
    std::map<std::string, size_t> index;
  };
  std::map<uint64_t, StepRec> steps_;
  // files
  struct Incoming {
    uint64_t total = 0, have = 0;
    SiteId from = 0;
    int tries = 0;
    double askedAt = -1;
    bool failed = false;
    bool verifying = false;
  };
  std::map<std::string, Incoming> incoming_;  // by "Audio/<name>"
  struct Outgoing {
    SiteId to = 0;
    std::string name;
    uint64_t offset = 0, size = 0;
  };
  std::deque<Outgoing> outgoing_;
  std::map<std::string, std::string> avatars_;  // hash -> bytes
  double lastFileScan_ = -1;
  double lastFlushMs_ = 0, lastApplyMs_ = 0;
  // membership (J-111e)
  struct Window {
    uint64_t upTo = 0;
    bool readd = false;
    SiteId readdBy = 0;
    uint64_t readdSeq = 0;
  };
  std::map<SiteId, std::vector<Window>> windows_;   // batches of a removed member that count no more
  std::map<SiteId, std::vector<Removal>> removals_;
  std::set<std::string> seenRemovals_, seenReadds_;
  std::set<SiteId> left_;
  std::map<SiteId, uint64_t> honouredTop_;           // newest batch of each author whose ops were applied
  std::map<SiteId, uint64_t> rebuildFor_;            // removals that cut off what we applied
  bool windowsFrozen_ = false, rebuildPending_ = false, tainted_ = false;
  double snapAskedAt_ = -1;
  uint64_t rebuilds_ = 0;
  ExitKind exit_ = ExitKind::None;
  bool exitPending_ = false;
  Removal myRemoval_;
  EndInfo end_;
  bool ended_ = false;
  uint64_t lastContact_ = 0;
  double contactSavedAt_ = -1;
};

// Deterministic repairs of a merged project JSON before it is loaded (sends to
// missing tracks, routing cycles, clips of missing patterns, out-of-range
// tempo): every peer derives the same song from the same replica.
void repairProjectJson(Json& song);
// Every plugin type of a project JSON that is not a native Gridline plugin,
// with where it is ("Channel 'Lead'").
std::vector<std::pair<std::string, std::string>> foreignPlugins(const Json& song);
// "Audio/<sha256>.<ext>" references in a text (JSON of ops, replica, undo records).
void audioRefsIn(const std::string& text, std::set<std::string>& out);

}  // namespace gl::collab

// Gridline: K-18: file operations of the browser (rename, new folder, move
// into a folder, move to the system trash) and what they mean for an open
// project: references (channels, note effects, mixer slots, through
// forEachFileRef) follow a renamed or moved file or folder, as one undo step
// that also moves the files back on undo. Sidecars that belong to a file
// ("<file>.origin.json", "<file>.source.json") travel with it.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "Song.h"
#include "Undo.h"

namespace gl::fileops {

using Move = std::pair<std::string, std::string>;  // from, to

// "" when `name` can be a file or folder name on every system Gridline runs
// on, else a short message for the user (empty, reserved characters, ".",
// "..", names Windows reserves, trailing dot or space, too long).
std::string checkName(const std::string& name);

// The suffixes of a file's sidecars and the ones that exist for `file`.
const std::vector<std::string>& sidecarSuffixes();
std::vector<std::string> sidecarsOf(const std::string& file);

// True when `p` is `dir` or lies inside it (compared lexically, normalised). Spellings that differ
// only in letter case are one path where the file system says so: Windows always, macOS when the
// volume ignores case, Linux never. The same goes for retargetPath and everything built on them.
bool isInside(const std::string& p, const std::string& dir);
// For paths already normalised: how many bytes of `p` spell `dir` (`p.size()` when `p` is `dir`,
// else `p[result]` is the separator after it), or npos when `p` is not `dir` nor inside it.
size_t matchedLength(const std::string& p, const std::string& dir);
// Tests: `rule` answers "does this folder's file system ignore case?" instead of the system (empty:
// the system again); remembered answers are forgotten.
void setCaseRuleForTests(std::function<bool(const std::string&)> rule);
// `p` equal to `from`, or inside folder `from`, becomes the same path under
// `to` (the part below `from` keeps its spelling). Returns true when it changed.
bool retargetPath(std::string& p, const std::string& from, const std::string& to);
// Every file reference of the song through the moves (each `from` -> `to`).
// Returns how many references changed.
int retargetSong(Song& song, const std::vector<Move>& moves);
// How many references of the song point at `path` (or into it, for a folder).
int referencesTo(const Song& song, const std::string& path);

struct Result {
  bool ok = false;
  std::string error;        // for the user, when !ok
  std::string path;         // the item's new path
  std::vector<Move> moves;  // the disk moves made, the item first, then its sidecars
};

// Renames a file or folder in place (`newName` is a name, not a path). A
// change of letter case only works on case-insensitive file systems too.
Result rename(const std::string& path, const std::string& newName);
// Moves a file or folder into `folder` (same name). Moving a folder into
// itself or one of its subfolders, or onto a name that exists, is refused.
Result moveInto(const std::string& path, const std::string& folder);
// Makes a new folder in `parent` named `baseName`, or "<baseName> 2", ... .
Result makeFolder(const std::string& parent, const std::string& baseName = "New folder");
// Moves a file or folder (and a file's sidecars) to the system trash: the
// Recycle Bin on Windows, ~/.Trash on macOS, the freedesktop.org trash
// elsewhere. `trashRoot` (tests): a freedesktop-style trash in that folder
// on every system instead.
Result moveToTrash(const std::string& path, const std::string& trashRoot = "");
// Undoes `moves` (last first). False with `err` when one cannot be undone.
bool moveBack(const std::vector<Move>& moves, std::string* err = nullptr);
// Makes the moves again (first first), each only when its source is there
// and its target is free.
bool moveAgain(const std::vector<Move>& moves, std::string* err = nullptr);

// One undo step for the project side of a rename / move: the song's
// references follow `refs` (top-level item moves); undo moves `disk` back on
// disk and the references with it, redo moves them again. `onMoved` (may be
// empty) hears every applied item move (from, to), e.g. to update an index.
class FileMoveCommand final : public Command {
 public:
  FileMoveCommand(std::string label, std::vector<Move> disk, std::vector<Move> refs,
                  std::function<void(const std::string&, const std::string&)> onMoved = {});
  void apply(Song& s) override;
  void revert(Song& s) override;
  std::string label() const override { return label_; }

 private:
  std::string label_;
  std::vector<Move> disk_, refs_;
  std::function<void(const std::string&, const std::string&)> onMoved_;
  bool first_ = true;  // the first apply: the files were moved already
};

}  // namespace gl::fileops

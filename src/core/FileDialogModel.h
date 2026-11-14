// Gridline: the Open / Save dialog's model, without any drawing (the dialog is app/FileDialogUi).
//
//   Volumes    the drives and volumes of this computer, per system: macOS the boot volume and
//              /Volumes, Linux the real file systems of /proc/self/mounts (and /media, /run/media,
//              /mnt), BSD getmntinfo, Windows the drive letters. Read on a worker thread: a slow or
//              network mount never holds up the interface (VolumeWatch).
//   Places     Home, Desktop, Documents, Music, Downloads (on Linux the XDG user folders).
//   Lists      favourites, recent folders and the last folder per dialog, as settings text.
//   Paths      breadcrumb parts (POSIX, Windows drive roots, UNC shares), Tab completion.
//   Entries    a folder's rows: kinds by name, sorted by a column with folders first.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gl::filedlg {

// ---- drives and volumes ----
enum class VolumeKind { System, Internal, Removable, Network, Optical };
struct Volume {
  std::string name;   // shown ("Macintosh HD", "USB STICK", "Data (D:)")
  std::string path;   // where it is mounted ("/", "/Volumes/USB", "D:\\")
  VolumeKind kind = VolumeKind::Internal;
  int64_t freeBytes = -1, totalBytes = -1;  // -1 = not known (network drives are never asked)
};
// Linux: /proc/self/mounts text ("device point type options 0 0", octal escapes in names). Real
// file systems only: the root, separate data partitions and anything under /media, /run/media and
// /mnt; never proc, sys, tmpfs, snap squashfs images, overlay, cgroup and other system mounts.
// `user` names /media/<user> and /run/media/<user> (those are removable). Free space is not filled.
std::vector<Volume> parseLinuxMounts(const std::string& mountsText, const std::string& user);
// The decision for one mount (Linux and BSD): kept, and of which kind.
bool keepMount(const std::string& device, const std::string& point, const std::string& type, VolumeKind* kind);
// This computer's volumes now, on the calling thread (may stat local disks; never a network one).
std::vector<Volume> detectVolumes();

// Volumes kept current from a worker thread. `poll` starts a new read when the last is older than
// `maxAge` seconds and returns the newest finished list (empty until the first is done). A read
// that hangs is left behind after a few seconds and a new one may start.
class VolumeWatch {
 public:
  std::shared_ptr<const std::vector<Volume>> poll(double maxAge = 2.0);
  // Tests: replaces the reader (nullptr = detectVolumes).
  static void setReaderForTests(std::vector<Volume> (*reader)());
  uint64_t reads() const;  // finished reads so far
 private:
  struct Cell;
  std::shared_ptr<Cell> cell_;
};

// ---- places ----
struct Place {
  std::string name;  // English label (translated where drawn)
  std::string path;
  int icon = 0;      // PlaceIcon
};
enum PlaceIcon { kIconHome, kIconDesktop, kIconDocuments, kIconMusic, kIconDownloads, kIconFolder };
// ~/.config/user-dirs.dirs text: XDG_DESKTOP_DIR="$HOME/Desktop" lines. Returns the folder for
// `key` ("DESKTOP", "DOCUMENTS", "MUSIC", "DOWNLOAD"), "" when not set.
std::string xdgUserDir(const std::string& text, const std::string& key, const std::string& home);
// Home, Desktop, Documents, Music, Downloads under `home` (Linux: the XDG names when set). Only
// names; the folders are never read here. `userDirsText` is the user-dirs file ("" = none).
std::vector<Place> standardPlaces(const std::string& home, const std::string& userDirsText);

// ---- favourites, recent folders, last folder per dialog (settings text, one entry a line) ----
std::vector<std::string> decodeList(const std::string& text);
std::string encodeList(const std::vector<std::string>& items);
bool contains(const std::vector<std::string>& items, const std::string& path);
// Pins (true) or unpins `path`; false when nothing changed.
bool setPinned(std::vector<std::string>& items, const std::string& path, bool pinned);
// Moves `path` to the front, at most `max` kept.
void pushRecent(std::vector<std::string>& items, const std::string& path, size_t max = 8);
// "purpose\tfolder" lines.
std::string lastFolder(const std::string& text, const std::string& purpose);
std::string setLastFolder(const std::string& text, const std::string& purpose, const std::string& folder, size_t max = 24);

// ---- paths ----
struct Crumb {
  std::string label;  // "/", "C:", "\\\\server\\share", "Music"
  std::string path;   // the folder this part opens
};
// "/home/me/Music" -> "/", "home", "me", "Music"; "C:\\Users\\me" -> "C:", "Users", "me";
// "\\\\server\\share\\a" -> "\\\\server\\share", "a". `windows` reads backslashes as separators.
std::vector<Crumb> splitBreadcrumb(const std::string& path, bool windows);
// Tab: the typed text completed against the names in its folder (the longest common start of
// every name that matches; a single folder gets its separator). `names` are the folder's entries,
// folders marked with a trailing '/'. Returns the text unchanged when nothing matches.
std::string completePath(const std::string& typed, const std::vector<std::string>& names, bool windows);
// The folder part and the last part of a typed path ("/a/b/mu" -> "/a/b/", "mu").
void splitTyped(const std::string& typed, bool windows, std::string* folder, std::string* rest);

// ---- entries ----
enum class FileKind { Folder, Project, Audio, Midi, Preset, Other };
FileKind kindOf(const std::string& name, bool isDir);
const char* kindLabel(FileKind k, const std::string& name);  // "Folder", "Project", "Audio", ...; else "<EXT> file" in a static buffer
enum class SortKey { Name, Size, Kind, Modified };
struct Entry {
  std::string name;
  bool isDir = false, hidden = false;
  int64_t size = -1, mtime = 0;
  FileKind kind = FileKind::Other;
};
// Folders first, then by the key (ties by name, natural order: "Take 2" before "Take 10").
void sortEntries(std::vector<Entry>& v, SortKey key, bool descending);
bool naturalLess(const std::string& a, const std::string& b);
// "1.2 MB", "640 KB", "12 bytes".
std::string sizeText(int64_t bytes);

}  // namespace gl::filedlg

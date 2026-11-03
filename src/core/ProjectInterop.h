// Gridline — project interop (PJ-04, FI): the files a project refers to
// (samples, impulse responses, images, audio editor takes), collecting them
// into a project folder ("export project data files"), project bones,
// zipped projects, relative paths, and finding / relinking missing files.
//
// File references live in plugin states, each plugin type in its own format
// (a bare path, a text model with paths on some of its lines, a JSON document,
// patches holding other plugin states). The walk in ProjectInterop.cpp knows
// them all and every function here goes through it, so they all agree on what
// a reference is: a callback sees a plain path ("sf2:<bank>#<n>" and
// "file:<path>" arrive as the file) and never sees a value that names no file
// ("tone:", "builtin:", "media:", "factory:", "none").
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Song.h"

namespace gl {

// True for strings that name a media / data file by extension (audio, MIDI,
// images, sound fonts, impulse responses, …).
bool looksLikeFilePath(const std::string& s);

// Calls `fn` for every file reference in the song (channels, note
// processors, mixer slots, all arrangements). `fn` may change the path; the
// state is rewritten when it does. Returns the number of references changed.
int forEachFileRef(Song& song, const std::function<void(std::string& path)>& fn);
// The file references of one plugin state (same rules as forEachFileRef).
// Returns true when `fn` changed one.
bool forEachStateFileRef(PluginState& state, const std::function<void(std::string& path)>& fn);
// The same with folder references too (a cabinet folder): `folder` tells them apart. For code that
// moves references as they are; a folder in a shared project is a manifest file (see below).
int forEachRef(Song& song, const std::function<void(std::string& path, bool folder)>& fn);
bool forEachStateRef(PluginState& state, const std::function<void(std::string& path, bool folder)>& fn);
// Unique referenced paths, in first-seen order.
std::vector<std::string> referencedFiles(const Song& song);
// Referenced files that do not exist (relative paths count as missing).
std::vector<std::string> missingFiles(const Song& song);
std::vector<std::string> missingAmong(const std::vector<std::string>& refs);  // the same over a list (from any thread)

// Relative references (e.g. "Samples/kick.wav" in a zipped project) become
// absolute against `baseDir` when the file exists there. Returns how many.
int resolveRelativePaths(Song& song, const std::string& baseDir);

// A multisample map (.sfz) refers to more files, by paths of any shape: relative through folders,
// through default_path, absolute, through #define, in #include files. A packed map is one flat text
// that names its samples directly, so it plays wherever it is put with its samples beside it.
struct SfzBundle {
  std::string text;                                          // the map, naming its members
  std::vector<std::pair<std::string, std::string>> members;  // source file -> name in the map
  std::vector<std::string> missing;                          // samples and includes that are not there
  bool ok = false;
};
// `memberName` chooses each sample's name (default: its file name, made unique).
SfzBundle sfzBundle(const std::string& path, const std::function<std::string(const std::string& source)>& memberName = {});

// A folder reference in a project whose files are stored by content (shared projects): a small file
// with this extension lists each file's real name and its stored name beside the manifest.
inline constexpr const char* kFolderManifestExt = ".cabinet";
bool isFolderManifest(const std::string& path);
std::string folderManifestText(const std::vector<std::pair<std::string, std::string>>& files);  // real name, stored name
// The files of a folder reference, a folder or a manifest: path, real name.
std::vector<std::pair<std::string, std::string>> folderRefFiles(const std::string& ref);
// File names a map or a manifest needs beside it. Empty for any other file.
std::vector<std::string> bundleMembers(const std::string& path);

// The sidecars of files that travel ("<file>.origin.json", "<file>.source.json"): target name ->
// text. `files` maps source -> target. An origin's sources follow: relative to the file when they
// travel too, else a bare name, and an unpacked link of a chain keeps its own sidecar under `freeName`.
std::map<std::string, std::string> sidecarsFor(const std::map<std::string, std::string>& files,
                                               const std::function<std::string(const std::string& fileName)>& freeName);

struct CollectReport {
  int copied = 0;                       // files copied
  int reused = 0;                       // identical file already in the folder
  std::vector<std::string> missing;     // references whose file does not exist
  // Files third-party plugins name inside their own saved state. They are packed too, but only the
  // plugin can change where it looks, so the person has to be told.
  std::vector<std::string> insidePlugins;
  // Paths to real files still in the written project: the check after every package. Empty unless a
  // state holds a reference in a form the walk does not know.
  std::vector<std::string> unpacked;
  std::map<std::string, std::string> moved;  // old path → new path
  std::string error;
};
// Copies every existing referenced file into `folder` (unique names; a file
// already there with the same content is reused) and points the references
// at the copies: absolute paths, or paths relative to `relativeTo` when given.
// A multisample map (.sfz) gets a folder of its own, with its samples beside it
// and the map rewritten to name them; a folder reference is copied as a folder.
CollectReport collectFiles(Song& song, const std::string& folder, const std::string& relativeTo = "");

// "Export project data files": `<folder>/<name>.gridline.json` + `<folder>/Samples/…`
// with relative references. The song in memory is not changed.
CollectReport exportProjectDataFiles(const Song& song, const std::string& folder, const std::string& name);
// "Export project bones": data files + one MIDI file per pattern and for the
// song (MIDI/), every channel's instrument state (Presets/) and every mixer
// track's effect chain (Presets/chains/).
struct BonesReport {
  CollectReport files;
  int midiFiles = 0, presets = 0, chains = 0;
};
BonesReport exportProjectBones(const Song& song, const std::string& folder, const std::string& name);
// Zipped project: the project (relative references) + its files in one .zip.
CollectReport exportZippedProject(const Song& song, const std::string& zipPath, const std::string& name);
// Absolute paths to files that exist on this computer, found anywhere in a project's text.
std::vector<std::string> absolutePathsInProject(const std::string& projectText);
// Extracts a zipped project into `dir` and returns the project file inside it
// (.gridline.json, or an FL Studio .flp) in `projectFile`.
bool unpackZippedProject(const std::string& zipPath, const std::string& dir, std::string& projectFile, std::string* err = nullptr);

// ---- missing files ----
struct RelinkOptions {
  bool matchStem = true;      // "kick.wav" may be found as "kick.flac"
  bool recursive = true;
  size_t maxFiles = 200000;   // stop indexing after this many files
  const std::atomic<bool>* cancel = nullptr;  // K-42: stops the walk
  std::map<std::string, uintmax_t> sizeHints;  // missing path → expected size (tie-break)
};
struct RelinkReport {
  std::map<std::string, std::string> found;  // missing path → replacement
  std::vector<std::string> notFound;
  size_t indexed = 0;
  bool shallow = false;  // K-42: `folder` is on a web or network drive: only that folder was searched
};
// Searches `folder` for each missing file by name (then by name without the
// extension); several candidates are ranked by matching parent folders, then
// by size hint. Does not change the song.
RelinkReport findReplacements(const std::vector<std::string>& missing, const std::string& folder, const RelinkOptions& o = {});
// Points every reference to a key of `map` at its value. Returns how many changed.
int relinkFiles(Song& song, const std::map<std::string, std::string>& map);

}  // namespace gl

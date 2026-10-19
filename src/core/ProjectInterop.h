// Gridline — project interop (PJ-04, FI): the files a project refers to
// (samples, impulse responses, images, audio editor takes), collecting them
// into a project folder ("export project data files"), project bones,
// zipped projects, relative paths, and finding / relinking missing files.
//
// File references live in plugin states: PluginState::data is either a bare
// path (sample-based plugins) or a JSON document whose string values may be
// paths. Every function here goes through forEachFileRef, so they all agree
// on what a reference is.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
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
// Unique referenced paths, in first-seen order.
std::vector<std::string> referencedFiles(const Song& song);
// Referenced files that do not exist (relative paths count as missing).
std::vector<std::string> missingFiles(const Song& song);

// Relative references (e.g. "Samples/kick.wav" in a zipped project) become
// absolute against `baseDir` when the file exists there. Returns how many.
int resolveRelativePaths(Song& song, const std::string& baseDir);

struct CollectReport {
  int copied = 0;                       // files copied
  int reused = 0;                       // identical file already in the folder
  std::vector<std::string> missing;     // references whose file does not exist
  std::map<std::string, std::string> moved;  // old path → new path
  std::string error;
};
// Copies every existing referenced file into `folder` (unique names; a file
// already there with the same content is reused) and points the references
// at the copies: absolute paths, or paths relative to `relativeTo` when given.
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
// Extracts a zipped project into `dir` and returns the project file inside it
// (.gridline.json, or an FL Studio .flp) in `projectFile`.
bool unpackZippedProject(const std::string& zipPath, const std::string& dir, std::string& projectFile, std::string* err = nullptr);

// ---- missing files ----
struct RelinkOptions {
  bool matchStem = true;      // "kick.wav" may be found as "kick.flac"
  bool recursive = true;
  size_t maxFiles = 200000;   // stop indexing after this many files
  std::map<std::string, uintmax_t> sizeHints;  // missing path → expected size (tie-break)
};
struct RelinkReport {
  std::map<std::string, std::string> found;  // missing path → replacement
  std::vector<std::string> notFound;
  size_t indexed = 0;
};
// Searches `folder` for each missing file by name (then by name without the
// extension); several candidates are ranked by matching parent folders, then
// by size hint. Does not change the song.
RelinkReport findReplacements(const std::vector<std::string>& missing, const std::string& folder, const RelinkOptions& o = {});
// Points every reference to a key of `map` at its value. Returns how many changed.
int relinkFiles(Song& song, const std::map<std::string, std::string>& map);

}  // namespace gl

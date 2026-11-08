// Gridline — K-13: the folders each plugin format is searched in.
//
// Per format, in this order:
//   Default   the OS's standard folders (Format::defaultPaths), one switch for all of them
//             (PluginPrefs::noDefaultFolders);
//   env       the format's environment variable (VST_PATH, VST3_PATH, CLAP_PATH, LV2_PATH), always on;
//   user      the user's folders of this format (PluginPrefs::formatFolders), each switchable;
//   every     the user's folders for every format (PluginPrefs::extraFolders), each switchable.
// A scan searches every folder that is on and exists. A folder listed twice is searched once
// (the first row); one inside another searched folder is marked so (its plugins are found once:
// discovery removes duplicate entries). Audio Units come from macOS's component registry: their
// folders are shown, never searched by Gridline, and no folder can be added.
#pragma once
#include <string>
#include <vector>
#include "PluginPrefs.h"
#include "PluginScan.h"

namespace gl::host {

enum class FolderSource { Default, Env, User, Every };

struct FolderRow {
  std::string format;  // format id
  std::string path;
  FolderSource source = FolderSource::Default;
  bool enabled = true;   // switched on (Default rows: the format's Default switch)
  bool exists = false;   // a folder that can be read
  bool duplicate = false;  // the same folder as an earlier row that is on (searched there)
  std::string inside;    // the searched folder this one lies in ("" = none)
  bool searched() const { return enabled && exists && !duplicate; }
};

// The format's environment variable for extra folders ("" for none).
const char* folderEnvVar(const std::string& formatId);
// False for formats whose plugins the OS registers (AU): their folders cannot be changed.
bool folderSearchApplies(const std::string& formatId);
// Every folder of a format as the settings list shows it.
std::vector<FolderRow> folderRows(const Format& f, const PluginPrefs& p);
// K-73: the same for a list the interface draws every frame. The folders are looked at on disk
// (exists, the same as another, inside another) again when the plugin settings change, and else
// every two seconds on a helper thread, so a slow folder (a network drive) never holds up a frame.
std::vector<FolderRow> folderRowsForDisplay(const Format& f, const PluginPrefs& p);
// The folders a scan searches (rows that are on, exist and are not duplicates), in order.
std::vector<std::string> searchFolders(const Format& f, const PluginPrefs& p);
// Same for a format id with the current settings (empty for an unknown format).
std::vector<std::string> searchFolders(const std::string& formatId);
// Entries of a format found in its search folders, each once (duplicates through nested
// folders, symbolic links or spelling are dropped).
std::vector<std::string> discoverFormat(const Format& f, const PluginPrefs& p);

// Key of a folder's switch in PluginPrefs::disabledFolders.
std::string folderKey(const std::string& formatIdOrStar, const std::string& path);
// Two spellings of the same folder ("/a/b/", "/a/./b"; case-insensitive on Windows and macOS).
bool sameFolder(const std::string& a, const std::string& b);
// True when `inner` lies inside `outer` (not the same folder).
bool folderInside(const std::string& inner, const std::string& outer);

}  // namespace gl::host

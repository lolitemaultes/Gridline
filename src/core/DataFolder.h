// Gridline - K-265: the data folder setting (Settings > Files > Data folder).
//
// Gridline keeps its settings (config), caches and downloaded data in three folders per system
// (Platform.h). The data folder setting moves all three under one chosen folder:
//   <folder>/config/gridline, <folder>/cache/gridline, <folder>/data/gridline
// It is kept in a small pointer file at the default place (<default config>/gridline/
// data-folder.txt, one line: the folder), so Gridline finds it before it reads any setting.
//  * Portable mode: a folder named "GridlineData" beside the program (beside Gridline.app on
//    macOS) is used without any pointer file, so Gridline runs from a stick with its own data.
//  * A chosen folder that is not there at start (an unplugged drive) is not created: Gridline
//    uses the default folders for that run and says so (missing = true).
//  * Environment variables still win: GRIDLINE_CONFIG_HOME turns the setting off; XDG_CONFIG_HOME,
//    XDG_CACHE_HOME and XDG_DATA_HOME each keep their own folder.
// The choice takes effect at the next start (the folders are read once per run).
#pragma once
#include <functional>
#include <string>

namespace gl::datafolder {

struct Info {
  std::string folder;       // the folder in use ("" = the default folders)
  std::string wanted;       // the folder the pointer file names (may be missing)
  bool portable = false;    // "GridlineData" beside the program
  bool missing = false;     // the pointer names a folder that is not there: defaults used
  bool envOverride = false; // GRIDLINE_CONFIG_HOME is set: the setting is off
};

inline constexpr const char* kPortableName = "GridlineData";

// Pure: the data folder for a default config root, the program's path and the environment.
Info resolve(const std::string& defaultConfigRoot, const std::string& exePath, bool envOverride);
// The pointer file below a default config root.
std::string pointerFile(const std::string& defaultConfigRoot);
// Writes (folder non-empty) or removes (empty) the pointer. False with `err` on failure.
bool writePointer(const std::string& defaultConfigRoot, const std::string& folder, std::string* err = nullptr);

// Copies the Gridline folders of the current roots (config / cache / data + "/gridline") into
// `folder` (config/gridline, cache/gridline, data/gridline), skipping the pointer file. The
// existing files stay where they are (a revert loses nothing). `progress(fraction, file)`
// returning false cancels. False with `err` on failure or cancel.
bool copyInto(const std::string& configRoot, const std::string& cacheRoot, const std::string& dataRoot, const std::string& folder,
              const std::function<bool(double, const std::string&)>& progress, std::string* err = nullptr);

}  // namespace gl::datafolder

// Gridline — the Gridline Suite plugin state as other programs keep it, with
// the files it uses carried inside (K-110).
//
// A suite plugin saves {"format":"gridline-suite","version":1,"plugin":<the
// plugin state exactly as a Gridline project holds it>, ...}. A project handed
// to another computer (an FL Studio project with the plugin in it, for one)
// cannot rely on the sample paths in "plugin": the files are elsewhere there,
// and a plugin never learns where its host's project lives. So the state may
// also carry the files themselves:
//   "files": [{"path": <the reference as the state names it>, "name": <file
//              name>, "size": <bytes>, "sha256": <hex>, "data": <base64>}]
// Older suite versions ignore the key and use the paths.
//
// On load, a reference whose file is there with the same size is kept. Any
// other reference with an embedded copy is pointed at that copy in the cache
// (<cache>/gridline/suite-files/<first 16 hex digits of its SHA-256>/<name>):
// the folder is named by the content, an existing file is reused only when its
// content matches, and nothing is ever written outside the cache. Writing the
// files is file I/O: callers run it on a host's main or loading thread, never
// the audio thread.
#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <vector>
#include "Json.h"
#include "PluginInfo.h"

namespace gl::suitestate {

inline constexpr const char* kFormat = "gridline-suite";
inline constexpr int kVersion = 1;
// Files embedded per plugin state, in total. Files past it stay references only.
inline constexpr size_t kEmbedLimit = size_t(64) << 20;

struct EmbedReport {
  int files = 0;
  size_t bytes = 0;
  std::vector<std::string> skipped;  // referenced, not embedded (too large, missing)
};

// Adds "files" to a suite state object `j` for the file references of `st`
// (each file once; in reference order until `limit` is reached). `only`: when
// non-null, only references in this set are embedded. Returns the count.
int embedFiles(const PluginState& st, Json& j, size_t limit, EmbedReport* rep = nullptr, const std::set<std::string>* only = nullptr);

// The suite state for a plugin state, as a suite plugin would save it, with
// its files embedded when `embed` is set.
std::string stateText(const PluginState& st, bool embed, EmbedReport* rep = nullptr);

// Restores the embedded files of a suite state object into the cache and
// points the references of `st` at them where the original file is not there.
// `restored` (optional) receives the references the state now names that have
// an embedded copy (kept or restored), so a later save can embed them again.
// Returns how many references were changed; `err` gets the first failure.
int restoreFiles(PluginState& st, const Json& j, const std::string& cacheDir, std::set<std::string>* restored = nullptr,
                 std::string* err = nullptr);

// <cache root>/gridline/suite-files
std::string defaultCacheDir();

// Parses a suite state: the plugin state (true), and the whole object in `whole` when given.
bool parse(const std::string& text, PluginState& st, Json* whole = nullptr);

}  // namespace gl::suitestate

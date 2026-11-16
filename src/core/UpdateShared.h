// Gridline - K-308: the update check shared by the app and the Gridline Suite plugins.
//
// One small state file, update-check.json in Gridline's config folder, so every plugin instance
// in every host and the app make at most one request a day between them. A process claims the
// day's request by creating update-check.lock (a folder, created atomically) and writing
// lastAttempt before it fetches; everyone else reads the cached answer. A stale lock (older than
// five minutes) is taken over. Writes go to a temporary file renamed over the old one.
//
// Pure file logic and rules, no network and no UI. The plugins' notice shows only a version and a
// Download button that opens the browser; nothing is ever installed by itself. Nothing personal is
// stored or sent. See docs/updates.md.
#pragma once
#include <functional>
#include <string>
#include "Update.h"

namespace gl::update::shared {

struct State {
  double lastAttempt = 0;  // seconds since the epoch: the last request started
  double lastSuccess = 0;  // ... the last answer arrived
  std::string latest;      // the newest version offered ("0.8.1"), "" when none was found
  std::string page;        // its release page
  std::string suiteUrl;    // the Gridline Suite installer for this system ("" = the page)
  std::string miniUrl;     // the Suite Mini installer for this system
  std::string skipVersion;  // "Skip this version"
  double remindAfter = 0;   // "Remind me later": not before this time
  bool pluginsAuto = true;  // the plugins check by themselves (off: only "Check now")
  bool beta = false;        // offer test versions (set from the app's setting)
  bool operator==(const State& o) const;
};

using Env = std::function<const char*(const char*)>;
// The folder of update-check.json (the app's config folder, core/Platform.h configRoot):
// $GRIDLINE_CONFIG_HOME/gridline, else $XDG_CONFIG_HOME/gridline, else
// ~/.config/gridline (macOS, Linux, BSD) or %APPDATA%\gridline (Windows). `windows` picks the rule.
std::string configDir(const Env& env, bool windows);
// Turned off for this process: GRIDLINE_NO_UPDATE_CHECK, CI, or a screenshot / test run.
bool disabledByEnv(const Env& env);

bool read(const std::string& dir, State& out);  // false: no file or not readable (out unchanged)
bool write(const std::string& dir, const State& s);

constexpr double kDay = 24 * 3600.0;
constexpr double kStaleLock = 5 * 60.0;
enum class Decision { Off, Cached, Fetch };
// A plugin's automatic check at `now`: Off when turned off, Cached when a request started in the
// last day (or a reminder is pending), Fetch otherwise.
Decision decide(const State& s, double now, const Env& env);
// Claims the day's request: true when this process made the lock (or took over a stale one,
// claimed more than kStaleLock seconds before `now`). `now` comes from the caller, so tests need
// no clock.
bool claim(const std::string& dir, double now);
void release(const std::string& dir);

// The notice: a newer version than `current`, not skipped, no reminder pending.
bool shouldShow(const State& s, const std::string& current, double now);

// One automatic check as a plugin (or the app) makes it, on a worker thread: decides, claims the
// day's request, fetches through `fetch` (one GET of the latest release), records the answer with
// this system's Suite and Mini installer links, and releases the claim. Returns the state after
// it (read from the file when another process fetched or the day's answer is cached). `forced`
// ("Check now") fetches even when a request ran today, still never twice at once.
State runCheck(const std::string& dir, double now, const Env& env, const Fetcher& fetch, const std::string& currentVersion,
               const Target& target, bool forced = false);

}  // namespace gl::update::shared

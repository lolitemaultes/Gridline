#pragma once
// J-108: conditions the app's own code reports for scripted runs. A dev-input script waits for
// one with "waitfor <name> <seconds>" (the script and --frames stop counting until the name is
// reached or the time is up) instead of guessing how many frames a slow machine needs.
// UI thread only.
#include <set>
#include <string>

namespace gl::devsig {

inline std::set<std::string>& reachedNames() {
  static std::set<std::string> s;
  return s;
}
inline void reach(const std::string& name) { reachedNames().insert(name); }
inline bool reached(const std::string& name) { return reachedNames().count(name) != 0; }
// J-110: "forget <name>": the next waitfor waits for the condition to be reported again.
inline void forget(const std::string& name) { reachedNames().erase(name); }

}  // namespace gl::devsig

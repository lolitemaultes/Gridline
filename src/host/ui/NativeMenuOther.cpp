// Gridline - the system menu bar (K-112) where there is none to fill (Windows, other systems):
// Gridline keeps its in-window menu row there.
#if !defined(__APPLE__) && !defined(__linux__)
#include "NativeMenu.h"

namespace gl::host::nativemenu {

bool available() { return false; }
std::string status() { return "no system menu bar on this system"; }
void setWindow(unsigned long) {}
void install(const std::vector<Item>&, const std::vector<Item>&) {}
void setSystemKeys(const std::vector<std::string>&) {}
void uninstall() {}
bool installed() { return false; }
void poll() {}
std::string takeChosen() { return {}; }
bool busStarted() { return false; }

}  // namespace gl::host::nativemenu
#endif

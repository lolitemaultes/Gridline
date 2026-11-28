// Gridline - the Linux global menu (K-112): the layout's menus exported as a dbusmenu
// (DBusMenuBus.h). available() is true only while something on the session bus shows global
// menus; everywhere else Gridline keeps its in-window menu row. All bus work runs on the
// dbusmenu::Worker's thread: nothing here waits on the bus. The worker starts only when a layout
// asks for the system menu bar (available(), install()): other runs never load libdbus.
#if defined(__linux__)
#include "DBusMenuBus.h"
#include "NativeMenu.h"

namespace gl::host::nativemenu {

namespace {
bool gStarted = false;
dbusmenu::Worker& worker() {  // starts it (once; returns at once)
  static dbusmenu::Worker w;
  w.start();
  gStarted = true;
  return w;
}
bool gInstalled = false;
unsigned long gXid = 0;
}  // namespace

bool available() { return worker().hostPresent(); }

std::string status() { return gStarted ? worker().status() : std::string("dbusmenu: not started (no layout asked for it)"); }

void setWindow(unsigned long xid) { gXid = xid; }
void setSystemKeys(const std::vector<std::string>&) {}  // the global menu binds no keys

void install(const std::vector<Item>& appMenu, const std::vector<Item>& menus) {
  worker().post(appMenu, menus, !gInstalled, gXid);
  gInstalled = true;
}

void uninstall() {
  if (!gInstalled || !gStarted) return;
  worker().post({}, {}, false, gXid);  // an empty menu: the host shows nothing for Gridline
  gInstalled = false;
}

bool installed() { return gInstalled; }

bool shown() { return gInstalled && gStarted && worker().hostShows(); }

void poll() {}  // the worker serves the bus

std::string takeChosen() { return gStarted ? worker().takeChosen() : std::string(); }

bool busStarted() { return gStarted; }

}  // namespace gl::host::nativemenu
#endif

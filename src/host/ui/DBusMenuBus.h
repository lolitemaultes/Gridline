// Gridline - the dbusmenu object on the session bus (K-112, Linux global menu).
//
// libdbus-1 is loaded at run time (dlopen), so Gridline links nothing and runs the same where it
// is missing. A private connection to the session bus carries a com.canonical.dbusmenu object at
// /com/gridline/menubar (GetLayout, GetGroupProperties, GetProperty, Event, EventGroup,
// AboutToShow, AboutToShowGroup, the Version / Status / TextDirection properties and
// Introspect), and the window is announced to whatever shows global menus:
//   - com.canonical.AppMenu.Registrar (RegisterWindow): KDE's appmenu module, the Unity, Budgie,
//     Xfce and MATE applets, the vala-panel registrar;
//   - KWin on X11 reads two window properties instead (_KDE_NET_WM_APPMENU_SERVICE_NAME and
//     _KDE_NET_WM_APPMENU_OBJECT_PATH), set through the Xlib SDL already loaded.
// Nothing on the bus to show it (GNOME without an extension, a Wayland session without the KDE
// protocol): available() is false and the app keeps its in-window menu row.
//
// Every bus call can wait on the bus (a name lookup, a flush), so the app never makes one: the
// Worker owns the connection on a thread of its own. The app hands it menu trees and reads back
// whether a host is there (watched as the bus announces names coming and going, never asked
// for) and what was chosen; none of its calls waits on the bus.
// Compiled on every Unix (the Linux build uses it; elsewhere only its tests do).
#pragma once
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "DBusMenuModel.h"

namespace gl::host::dbusmenu {

class Bus {
 public:
  Bus() = default;
  ~Bus();
  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;

  // Loads libdbus, connects to the session bus and puts the object there. False (with
  // status() saying why) when any step is missing.
  bool open(const char* library = nullptr);
  void close();
  bool isOpen() const { return conn_ != nullptr; }
  // True when something on the bus shows global menus (a registrar, or KWin's appmenu module).
  // Asks the bus and waits for the answers: the worker's first look only.
  bool hasHost();
  // Watches the hosts' names (the bus's NameOwnerChanged signals; nothing waits): hostSeen()
  // follows them as they come and go.
  bool watchHosts();
  bool hostSeen() const { return hosts_.present(); }
  // A host has read the menus while there (HostState in DBusMenuModel.h): it shows them.
  bool hostShows() const { return hosts_.shows(); }
  // A host appeared since the last call (the window is announced to it again).
  bool takeHostAppeared();
  // One step: reads, writes and dispatches what is waiting, for up to `timeoutMs`.
  void step(int timeoutMs);
  // Announces the menus for the X11 window `xid` (0: only the registrar's "active window" use).
  void announce(unsigned long xid);
  // New contents: emits LayoutUpdated when they changed.
  void update(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus);
  // Handles the messages waiting, without blocking.
  void poll();
  std::string takeChosen();
  const std::string& status() const { return status_; }
  const Model& model() const { return model_; }
  std::string uniqueName() const;

  // Tests (a stand-in host and a menu bar reading the menus): own a well-known name, give it up,
  // and call the menu object of `dest` (no reply awaited).
  bool requestName(const char* name);
  bool releaseName(const char* name);
  bool sendEvent(const std::string& dest, int id, const char* event);
  bool sendGetLayout(const std::string& dest);

  // The bus calls these for messages to the object and for the signals it watches (public for
  // the C callbacks).
  int handle(void* msg);
  int filter(void* msg);

 private:
  void* lib_ = nullptr;
  void* conn_ = nullptr;
  Model model_;
  std::string status_ = "not started";
  std::string chosen_;
  std::string registrar_;
  HostState hosts_;  // the host names that have an owner now, and whether one read the menus
  bool appeared_ = false;
  bool watching_ = false;
};

// The bus on a thread of its own (see the top). start() returns at once; every other call takes
// a lock held only to copy a few values, never across a bus call.
class Worker {
 public:
  Worker() = default;
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // Starts the thread (once): it loads libdbus, connects, watches the hosts and serves the menus.
  void start(const char* library = nullptr);
  void stop();
  // Something on the bus could show global menus (as last seen by the worker).
  bool hostPresent() const { return host_.load(); }
  // ... and has read the menus, so it shows them (GitHub #85: only then the app's menu row goes).
  bool hostShows() const { return shows_.load(); }
  // The connection is up.
  bool connected() const { return up_.load(); }
  std::string status() const;
  std::string uniqueName() const;
  // A new tree to serve (copied); `announce`: the window `xid` is announced to the host now.
  void post(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus, bool announce, unsigned long xid);
  std::string takeChosen();

 private:
  void run(std::string library);
  std::thread thread_;
  std::atomic<bool> stop_{false}, host_{false}, shows_{false}, up_{false};
  mutable std::mutex m_;
  std::string status_ = "not started", unique_, chosen_;
  bool pending_ = false, announce_ = false;
  unsigned long xid_ = 0;
  std::vector<nativemenu::Item> app_, menus_;
};

}  // namespace gl::host::dbusmenu

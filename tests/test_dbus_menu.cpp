// Gridline - K-112: the Linux global menu's model (host/ui/DBusMenuModel.h): the menu tree as
// dbusmenu entries (ids, labels with escaped mnemonics, separators, check marks, shortcuts,
// submenus), ids that stay while items stay, the revision, and clicks mapped back to the app's
// item ids. Also the bus part where libdbus is missing: it says so and nothing is shown.
#include "host/ui/DBusMenuModel.h"
#if !defined(_WIN32)
#include "host/ui/DBusMenuBus.h"
#endif
#include "Check.h"
#if !defined(_WIN32)
#include <signal.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#endif

using namespace gl::host;
using nativemenu::Item;

namespace {
Item item(const std::string& label, const std::string& id, const std::string& shortcut = "") {
  Item i;
  i.label = label;
  i.id = id;
  i.shortcut = shortcut;
  return i;
}
Item menu(const std::string& label, std::vector<Item> children) {
  Item i;
  i.label = label;
  i.children = std::move(children);
  return i;
}

#if !defined(_WIN32)
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Waits up to `ms` for `ok`, the calls it makes timed (the slowest is kept).
template <class F>
bool within(int ms, double& slowest, F&& ok) {
  const auto t0 = Clock::now();
  while (msSince(t0) < ms) {
    const auto c = Clock::now();
    const bool r = ok();
    slowest = std::max(slowest, msSince(c));
    if (r) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// The worker against a session bus of its own (dbus-daemon started by the test), with a
// stand-in host and a stand-in menu bar. Skipped where there is no dbus-daemon or no libdbus.
void mockBus() {
  if (std::system("command -v dbus-daemon >/dev/null 2>&1") != 0) {
    std::printf("skipped: the mock session bus (no dbus-daemon)\n");
    return;
  }
  FILE* p = popen("dbus-daemon --session --fork --nopidfile --print-address=1 --print-pid=1 2>/dev/null", "r");
  if (!p) {
    std::printf("skipped: the mock session bus (dbus-daemon did not start)\n");
    return;
  }
  char addr[512] = {}, pidText[64] = {};
  const bool got = std::fgets(addr, sizeof addr, p) && std::fgets(pidText, sizeof pidText, p);
  pclose(p);
  const int pid = std::atoi(pidText);
  if (!got || pid <= 0) {
    std::printf("skipped: the mock session bus (no address)\n");
    return;
  }
  std::string address = addr;
  while (!address.empty() && (address.back() == '\n' || address.back() == '\r')) address.pop_back();
  setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);  // this test's own bus, never the desktop's
  {
    dbusmenu::Worker w;
    double slowest = 0;
    auto t = Clock::now();
    w.start();
    slowest = std::max(slowest, msSince(t));
    if (!within(3000, slowest, [&] { return w.connected() || w.status().rfind("no ", 0) == 0; }) || !w.connected()) {
      std::printf("skipped: the mock session bus (the worker did not connect: %s)\n", w.status().c_str());
      w.stop();
      ::kill(pid, SIGTERM);
      return;
    }
    expect(!w.hostPresent(), "mock bus: no host yet");
    // A host appears: the worker sees it from the bus's own signal.
    dbusmenu::Bus host;
    expect(host.open() && host.requestName("com.canonical.AppMenu.Registrar"), "mock bus: the stand-in host owns the registrar name");
    expect(within(3000, slowest, [&] { host.step(0); return w.hostPresent(); }), "mock bus: the host is seen without asking");
    // GitHub #85: a name on the bus is not a menu on screen. Until the host reads the menus, the
    // app keeps its own menu row.
    w.post({}, {menu("File", {item("New", "file-new")})}, true, 0);
    for (int i = 0; i < 30; ++i) {
      host.step(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(w.hostPresent() && !w.hostShows(), "mock bus: a host that never read the menus does not show them (#85)");
    // A tree with labels that are not UTF-8 (libdbus would end the program on them unsanitised),
    // read by the stand-in menu bar, then an item clicked there.
    std::vector<Item> bad = {menu("File\xC3", {item("New\xFF\xFEone", "file-new"), item("Open", "file-open")})};
    t = Clock::now();
    w.post({}, bad, true, 0);
    slowest = std::max(slowest, msSince(t));
    const std::string dest = w.uniqueName();
    expect(!dest.empty(), "mock bus: the worker has a name on the bus");
    host.sendGetLayout(dest);
    expect(within(3000, slowest, [&] { host.step(0); return w.hostShows(); }), "mock bus: the host read the menus: they are shown (#85)");
    // root 0, File 1, New 2. post() hands the tree to the worker's thread, which serves it after its
    // current bus step: a click read in that same step finds no item 2 yet and is dropped (a real menu
    // bar only clicks what it was served). So the click is sent again every 100 ms until it comes back.
    std::string chosen;
    auto lastClick = Clock::now() - std::chrono::seconds(1);
    expect(within(3000, slowest,
                  [&] {
                    if (msSince(lastClick) >= 100) {
                      host.sendEvent(dest, 2, "clicked");
                      lastClick = Clock::now();
                    }
                    host.step(0);
                    chosen = w.takeChosen();
                    return !chosen.empty();
                  }) &&
               chosen == "file-new",
           "mock bus: a click in the menu bar comes back (the worker served the bad labels and lives)");
    // The host goes away.
    host.releaseName("com.canonical.AppMenu.Registrar");
    expect(within(3000, slowest, [&] { host.step(0); return !w.hostPresent(); }), "mock bus: the host leaving is seen");
    expect(!w.hostShows(), "mock bus: with the host gone the menus are not shown (#85)");
    t = Clock::now();
    (void)w.status();
    (void)w.takeChosen();
    slowest = std::max(slowest, msSince(t));
    expect(slowest < 5.0, ("mock bus: no call from the app waits on the bus (slowest " + std::to_string(slowest) + " ms)").c_str());
    std::printf("mock bus: ran (slowest app call %.3f ms)\n", slowest);
    host.close();
    w.stop();
  }
  ::kill(pid, SIGTERM);
}
#endif
}  // namespace

int main() {
  // ---- helpers ----
  expect(dbusmenu::escapeMnemonic("Save_as") == "Save__as", "underscores doubled");
  expect(dbusmenu::escapeMnemonic("Plain") == "Plain", "plain label kept");
  using Keys = std::vector<std::vector<std::string>>;
  expect(dbusmenu::shortcutKeys("Ctrl+Shift+S") == Keys{{"Control", "Shift", "S"}}, "Ctrl+Shift+S");
  expect(dbusmenu::shortcutKeys("F9") == Keys{{"F9"}}, "F9");
  expect(dbusmenu::shortcutKeys("Ctrl++") == Keys{{"Control", "+"}}, "Ctrl++ (the plus key)");
  expect(dbusmenu::shortcutKeys("Alt+Super+K") == Keys{{"Alt", "Super", "K"}}, "Alt+Super+K");
  expect(dbusmenu::shortcutKeys("").empty(), "no shortcut");

  // ---- valid UTF-8 (libdbus ends the program on anything else) ----
  expect(dbusmenu::validUtf8("Grüße 漢字 🎹") == "Grüße 漢字 🎹", "valid UTF-8 kept");
  expect(dbusmenu::validUtf8("a\xFF" "b") == "a\xEF\xBF\xBD" "b", "a stray byte replaced");
  expect(dbusmenu::validUtf8("\xC3") == "\xEF\xBF\xBD", "a character cut short replaced");
  expect(dbusmenu::validUtf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD", "an overlong form replaced");
  expect(dbusmenu::validUtf8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD", "a surrogate replaced");
  expect(dbusmenu::validUtf8(std::string("a\0b", 3)) == "ab", "NUL dropped");
  {
    dbusmenu::Model bad;
    bad.set({}, {menu("Fi\xFF" "le", {item("x\xC3", "x")})});
    const dbusmenu::Entry* f = bad.entry(bad.entry(0)->children[0]);
    expect(f && f->label == "Fi\xEF\xBF\xBD" "le", "a label that is not UTF-8 is made valid");
  }

  // ---- GitHub #85: a global menu host shows the menus only once it has read them ----
  {
    dbusmenu::HostState h;
    expect(!h.present() && !h.shows(), "host state: nothing at first");
    h.menusRead();
    expect(!h.shows(), "host state: a read with no host there proves nothing");
    h.nameOwner("com.canonical.AppMenu.Registrar", true);
    expect(h.present() && !h.shows(), "host state: a registrar on the bus alone does not show the menus");
    h.menusRead();
    expect(h.shows() && h.first() == "com.canonical.AppMenu.Registrar", "host state: the host read the menus: shown");
    h.nameOwner("org.kde.kappmenu", true);
    h.nameOwner("com.canonical.AppMenu.Registrar", false);
    expect(h.shows() && h.first() == "org.kde.kappmenu", "host state: one host of two left: still shown");
    h.nameOwner("org.kde.kappmenu", false);
    expect(!h.present() && !h.shows(), "host state: every host gone: not shown");
    h.nameOwner("com.canonical.AppMenu.Registrar", true);
    expect(!h.shows(), "host state: a host back again must read the menus again");
    h.clear();
    expect(!h.present() && !h.shows(), "host state: cleared");
  }

  // ---- the tree ----
  Item mixer = item("Mixer", "view\x1fMixer", "F9");
  mixer.checked = true;
  Item off = item("Paste", "edit\x1fPaste");
  off.enabled = false;
  std::vector<Item> menus = {
      menu("File", {item("New", "file\x1fNew", "Ctrl+N"), Item{}, menu("Recent", {item("a_b.gridline", "file\x1fRecent\x1f" "a")})}),
      menu("Edit", {off}),
      menu("View", {mixer}),
  };
  std::vector<Item> app = {item("About Gridline", "app\x1f" "About")};
  dbusmenu::Model m;
  expect(m.set(app, menus), "first set changes");
  const uint32_t rev = m.revision();
  const dbusmenu::Entry* root = m.entry(0);
  expect(root && root->submenu && root->children.size() == 4, "root: the app menu then three menus");
  const dbusmenu::Entry* file = m.entry(root->children[1]);
  expect(file && file->label == "File" && file->submenu && file->children.size() == 3, "File: three entries");
  const dbusmenu::Entry* sep = m.entry(file->children[1]);
  expect(sep && sep->separator && sep->label.empty(), "separator entry");
  const dbusmenu::Entry* recent = m.entry(file->children[2]);
  expect(recent && recent->submenu && recent->children.size() == 1, "nested submenu");
  const dbusmenu::Entry* rf = m.entry(recent->children[0]);
  expect(rf && rf->label == "a__b.gridline" && rf->parent == recent->id, "escaped nested label and parent");
  const dbusmenu::Entry* newE = m.entry(file->children[0]);
  expect(newE && newE->shortcut == Keys{{"Control", "N"}} && !newE->checkable, "shortcut and no check mark");
  const dbusmenu::Entry* mixE = m.entry(m.entry(root->children[3])->children[0]);
  expect(mixE && mixE->checkable && mixE->checked, "checked item");
  const dbusmenu::Entry* pasteE = m.entry(m.entry(root->children[2])->children[0]);
  expect(pasteE && !pasteE->enabled, "disabled item");
  expect(m.appIdOf(newE->id) == "file\x1fNew", "click maps to the app id");
  expect(m.appIdOf(file->id).empty() && m.appIdOf(sep->id).empty() && m.appIdOf(9999).empty(), "submenus, separators and unknown ids map to nothing");

  // Same tree: no change, same ids.
  const int newId = newE->id, mixId = mixE->id;
  expect(!m.set(app, menus) && m.revision() == rev, "same tree: no new revision");
  // A state change: a new revision, the same ids.
  menus[2].children[0].checked = false;
  expect(m.set(app, menus) && m.revision() == rev + 1, "unchecked: new revision");
  expect(m.entry(mixId) && !m.entry(mixId)->checked && m.entry(newId) && m.entry(newId)->label == "New", "ids kept across rebuilds");
  // An item added before others keeps the others' ids.
  menus[0].children.insert(menus[0].children.begin(), item("Open", "file\x1fOpen"));
  expect(m.set(app, menus), "added item: changed");
  expect(m.entry(newId) && m.entry(newId)->label == "New", "existing id stays on its item");
  // Two items of one name are told apart.
  menus[1].children = {item("Empty", "e\x1f" "0"), item("Empty", "e\x1f" "1")};
  m.set(app, menus);
  const dbusmenu::Entry* edit = m.entry(m.entry(0)->children[2]);
  expect(edit && edit->children.size() == 2 && edit->children[0] != edit->children[1], "same names, two ids");
  expect(m.appIdOf(edit->children[1]) == "e\x1f" "1", "second of the same name");
  // Empty: only the root.
  expect(m.set({}, {}) && m.entry(0) && m.entry(0)->children.empty(), "an empty menu bar");

#if !defined(_WIN32)
  // ---- the bus, where the library is missing ----
  dbusmenu::Bus bus;
  expect(!bus.open("libgridline-no-such-dbus.so") || bus.isOpen(), "open with a missing library does not crash");
  if (!bus.isOpen()) expect(!bus.hasHost() && !bus.status().empty(), "no bus: no host, and a reason");
  bus.poll();
  expect(bus.takeChosen().empty(), "nothing chosen");
  mockBus();
#endif
  return finish();
}

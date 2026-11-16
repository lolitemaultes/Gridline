// Gridline - a layout's menus in the operating system's menu bar (K-112). See SystemMenu.h.
#include "SystemMenu.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Keymap.h"
#include "../core/LayoutStyle.h"
#include "../core/MenuTree.h"
#include "../core/Settings.h"
#include "../host/ui/NativeMenu.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LayoutStyleUi.h"
#include "MenuTreeUi.h"

namespace gl::sysmenu {

namespace nm = host::nativemenu;

namespace {

bool gForce = false, gHidden = false;
std::vector<nm::Item> gApp, gMenus;  // the tree last installed
uint64_t gHash = 0;
int gLastWalk = -1000;
uint64_t gSeen = 0;  // what the menus show was last read at this state signature
std::string gPending;  // an id chosen (system menu bar or dev), run on the next frame
layout::Preset gStyle = layout::Preset::Studio;

constexpr char kSep = '\x1f';

// The item's id: "<root>\x1f<submenu>...\x1f<label>\x1f<occurrence>" (raw labels).
std::string idOf(int root, const std::vector<std::string>& path, const menutree::Node& n) {
  std::string id = std::to_string(root);
  for (const std::string& p : path) id += kSep + p;
  return id + kSep + n.raw + kSep + std::to_string(n.occ);
}

void convert(const menutree::Node& n, int root, std::vector<std::string>& path, std::vector<nm::Item>& out) {
  if (n.separatorBefore && !out.empty() && !out.back().label.empty()) out.push_back(nm::Item{});
  nm::Item it;
  it.label = n.shown;
  it.enabled = n.enabled;
  if (n.submenu) {
    path.push_back(n.raw);
    for (const menutree::Node& c : n.children) convert(c, root, path, it.children);
    path.pop_back();
    while (!it.children.empty() && it.children.back().label.empty()) it.children.pop_back();
    if (it.children.empty()) return;  // an empty submenu
  } else {
    it.shortcut = n.shortcut;
    it.checked = it.checkable = n.checked;
    it.id = idOf(root, path, n);
  }
  out.push_back(std::move(it));
}

void walk(App& a, layout::Preset style, std::vector<nm::Item>& app, std::vector<nm::Item>& menus) {
  const menutree::Tree t = menutreeui::build(a, style);
  for (size_t r = 0; r < t.roots.size(); ++r) {
    const menutree::Node& root = t.roots[r];
    std::vector<std::string> path;
    const bool program = root.raw == "Gridline";  // the program menu (the style's unnamed one)
    if (program) {
      for (const menutree::Node& c : root.children) convert(c, static_cast<int>(r), path, app);
      continue;
    }
    nm::Item top;
    top.label = root.shown;
    for (const menutree::Node& c : root.children) convert(c, static_cast<int>(r), path, top.children);
    if (!top.children.empty()) menus.push_back(std::move(top));
  }
}

uint64_t hashOf(const std::vector<nm::Item>& v, uint64_t h) {
  auto mix = [&](const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    h = (h ^ 0x1f) * 1099511628211ull;
  };
  for (const nm::Item& it : v) {
    mix(it.label);
    mix(it.shortcut);
    mix(it.id);
    h = (h ^ (it.enabled ? 1u : 2u) ^ (it.checked ? 4u : 8u)) * 1099511628211ull;
    h = hashOf(it.children, h);
  }
  return h;
}

// A cheap signature of what the menus reflect: the project (every edit, save and load), the
// selection, the open windows, the keys, the transport, the clipboards and the recent files.
// The menu code runs (a walk) only when it moved, after an item was chosen, and every few
// seconds for anything else.
uint64_t stateSignature(App& a, layout::Preset style) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(a.doc.revision());
  mix(a.doc.saveSerial());
  mix(a.doc.projectEpoch());
  mix(keys::keymap().version());
  mix(static_cast<uint64_t>(style));
  mix(a.selChannel);
  mix(a.selMixer);
  mix(a.curPattern);
  mix(a.settingsChannel);
  mix(a.fxEditor.first);
  mix(static_cast<uint64_t>(a.fxEditor.second + 1));
  mix(a.engine.playing() ? 1 : 0);
  mix(a.piano.clipboard.size());
  mix(a.playlist.clipboard.size());
  mix(a.playlist.selected.size());
  mix(a.recentFiles.size());
  if (!a.recentFiles.empty())
    for (unsigned char c : a.recentFiles.front()) mix(c);
  uint64_t bits = 0;
  int i = 0;
  for (const layoutui::WindowFlag& f : layoutui::windowFlags(a)) bits ^= (*f.open ? 1ull : 0ull) << (i++ % 64);
  mix(bits);
  return h;
}

bool wanted(App& a) {
  if (gHidden && !gForce) return false;  // test runs keep the in-window row
  if (a.home.visible()) return false;
  if (!gForce && !a.settings.getBool("ui.systemMenuBar")) return false;
  // Studio keeps its menus in its top bar; the layouts built from their shell with a menu row
  // of their own put them in the system bar.
  const layout::Preset p = layoutui::active();
  if (!layoutui::shellBuilt(a, p) || layoutui::style().menus != layout::MenuPlace::OwnRow) return false;
  static int lastAsk = -1000;
  static bool avail = false;
  const int f = ImGui::GetFrameCount();
  if (f - lastAsk >= 60) {  // a host that appears later (Linux) is found within a second
    lastAsk = f;
    avail = nm::available();
  }
  return avail;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out(1);
  for (char c : s)
    if (c == sep) out.emplace_back();
    else out.back() += c;
  return out;
}

void runPending(App& a) {
  if (gPending.empty()) return;
  gSeen = 0;  // what an item does shows in the menus at once
  const std::vector<std::string> parts = split(gPending, kSep);
  gPending.clear();
  if (parts.size() < 3) return;
  const int root = std::atoi(parts[0].c_str());
  std::vector<std::string> path(parts.begin() + 1, parts.end() - 2);
  const std::string label = parts[parts.size() - 2];
  const int occ = std::atoi(parts.back().c_str());
  const menutreeui::RunResult r = menutreeui::run(a, gStyle, root, path, label, occ);
  if (r == menutreeui::RunResult::Gone) a.notify("That menu item is no longer there", true);
}

void dumpInto(const std::vector<nm::Item>& v, int depth, const std::string& at, std::string& out) {
  for (const nm::Item& it : v) {
    if (it.label.empty()) {
      out += std::string(static_cast<size_t>(depth) * 2, ' ') + "---\n";
      continue;
    }
    out += std::string(static_cast<size_t>(depth) * 2, ' ') + at + it.label + "|" + it.shortcut + "|" + (it.enabled ? "on" : "off") +
           (it.checked ? "|checked" : "") + "\n";
    dumpInto(it.children, depth + 1, "", out);
  }
}

std::string idAt(const std::vector<nm::Item>& v, const std::vector<std::string>& path, size_t i) {
  for (const nm::Item& it : v)
    if (!it.label.empty() && it.label == path[i]) {
      if (i + 1 == path.size()) return it.id;
      return idAt(it.children, path, i + 1);
    }
  return {};
}

}  // namespace

bool active() { return nm::installed(); }
void setDevForce(bool on) { gForce = on; }
void setHiddenRun(bool hidden) { gHidden = hidden; }
bool hiddenRun() { return gHidden; }
void setWindow(unsigned long xid) { nm::setWindow(xid); }

void frame(App& a) {
  nm::poll();
  if (std::string c = nm::takeChosen(); !c.empty()) gPending = c;
  runPending(a);
  if (!wanted(a)) {
    if (nm::installed()) nm::uninstall();
    gHash = 0;
    return;
  }
  // The menus change with the project (recent files, checked items, what can run now): read
  // twice a second, installed again only when something changed.
  const int f = ImGui::GetFrameCount();
  const layout::Preset style = layoutui::menuStyle(a);
  const uint64_t sig = stateSignature(a, style);
  if (nm::installed() && style == gStyle && sig == gSeen && f - gLastWalk < 240) return;
  gSeen = sig;
  gLastWalk = f;
  gStyle = style;
  std::vector<nm::Item> app, menus;
  walk(a, style, app, menus);
  // The system bar's own keys (macOS Hide): only those the active key set leaves free.
  std::vector<std::string> sysKeys;
  for (const keys::SystemMenuKey& k : keys::systemMenuKeys(keys::keymap())) sysKeys.emplace_back(k.item);
  uint64_t h = hashOf(menus, hashOf(app, 1469598103934665603ull));
  for (const std::string& k : sysKeys)
    for (unsigned char c : k) h = (h ^ c) * 1099511628211ull;
  if (nm::installed() && h == gHash) return;
  gHash = h;
  gApp = std::move(app);
  gMenus = std::move(menus);
  nm::setSystemKeys(sysKeys);
  nm::install(gApp, gMenus);
}

std::string dump() {
  std::string out = std::string(nm::installed() ? "installed" : "not installed") + " (" + nm::status() + ")\n";
  dumpInto(gApp, 0, "[app] ", out);
  dumpInto(gMenus, 0, "", out);
  return out;
}

bool choose(const std::string& path) {
  const std::vector<std::string> parts = split(path, '/');
  std::string id = idAt(gMenus, parts, 0);
  if (id.empty()) id = idAt(gApp, parts, 0);
  if (id.empty()) return false;
  gPending = id;
  return true;
}

bool devCommand(App&, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return false;
  if (args[0] == "dump") {
    std::printf("SYSMENU BEGIN frame %d\n%sSYSMENU END\n", frame, dump().c_str());
  } else if (args[0] == "status") {
    // Read only (asking available() would start the Linux bus worker).
    std::printf("frame %d sysmenu status installed %d bus %d (%s)\n", frame, nm::installed() ? 1 : 0, nm::busStarted() ? 1 : 0,
                nm::status().c_str());
  } else if (args[0] == "choose" && args.size() > 1) {
    std::string path = args[1];
    for (size_t i = 2; i < args.size(); ++i) path += " " + args[i];
    std::printf("frame %d sysmenu choose %s %s\n", frame, path.c_str(), choose(path) ? "ok" : "missing");
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::sysmenu

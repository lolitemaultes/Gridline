// Gridline — the browser's tabs: see BrowserTabs.h.
#include "BrowserTabs.h"
#include <algorithm>
#include <sstream>

namespace gl::browse {

namespace {
constexpr const char* kHeader = "gridline-browser-tabs 1";
constexpr TabKind kBuiltIn[kBuiltInTabs] = {TabKind::All, TabKind::Project, TabKind::Plugins, TabKind::Sounds, TabKind::Starred};

int builtInOrder(TabKind k) {
  for (int i = 0; i < kBuiltInTabs; ++i)
    if (kBuiltIn[i] == k) return i;
  return -1;
}

// A folder path as compared and saved: no trailing separator (except the root itself).
std::string tidyPath(std::string p) {
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\') && !(p.size() == 3 && p[1] == ':')) p.pop_back();
  return p;
}

std::string clean(std::string s) {  // a saved field: no tabs or line breaks inside it
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}
}  // namespace

std::vector<TabEntry> defaultTabs() {
  std::vector<TabEntry> v;
  for (TabKind k : kBuiltIn) v.push_back(TabEntry{k, {}, {}});
  return v;
}

bool isDefault(const std::vector<TabEntry>& tabs) { return tabs == defaultTabs(); }

const char* tabId(TabKind k) {
  switch (k) {
    case TabKind::All: return "all";
    case TabKind::Project: return "project";
    case TabKind::Plugins: return "plugins";
    case TabKind::Sounds: return "sounds";
    case TabKind::Starred: return "starred";
    case TabKind::Folder: break;
  }
  return "";
}

std::string tabsToText(const std::vector<TabEntry>& tabs) {
  std::string out = std::string(kHeader) + "\n";
  for (const TabEntry& t : tabs) {
    if (t.kind == TabKind::Folder) out += "folder\t" + clean(t.path) + "\t" + clean(t.name) + "\n";
    else out += std::string(tabId(t.kind)) + "\n";
  }
  return out;
}

std::vector<TabEntry> tabsFromText(const std::string& text) {
  std::vector<TabEntry> tabs;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line == kHeader || line.rfind("gridline-browser-tabs", 0) == 0) continue;
    const size_t t1 = line.find('\t');
    const std::string id = line.substr(0, t1);
    if (id == "folder") {
      if (t1 == std::string::npos) continue;
      const size_t t2 = line.find('\t', t1 + 1);
      const std::string path = tidyPath(line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1));
      const std::string name = t2 == std::string::npos ? std::string() : line.substr(t2 + 1);
      if (path.empty() || findTab(tabs, TabKind::Folder, path) >= 0) continue;
      tabs.push_back(TabEntry{TabKind::Folder, path, name});
      continue;
    }
    for (TabKind k : kBuiltIn)
      if (id == tabId(k) && findTab(tabs, k) < 0) tabs.push_back(TabEntry{k, {}, {}});
  }
  if (tabs.empty()) return defaultTabs();
  return tabs;
}

int findTab(const std::vector<TabEntry>& tabs, TabKind k, const std::string& path) {
  const std::string want = k == TabKind::Folder ? tidyPath(path) : std::string();
  for (size_t i = 0; i < tabs.size(); ++i)
    if (tabs[i].kind == k && (k != TabKind::Folder || tabs[i].path == want)) return static_cast<int>(i);
  return -1;
}

int addFolderTab(std::vector<TabEntry>& tabs, const std::string& path, const std::string& name, int at) {
  const std::string p = tidyPath(path);
  if (p.empty()) return -1;
  const int have = findTab(tabs, TabKind::Folder, p);
  if (have >= 0) return have;
  const size_t pos = at < 0 ? tabs.size() : std::min(static_cast<size_t>(at), tabs.size());
  tabs.insert(tabs.begin() + static_cast<long>(pos), TabEntry{TabKind::Folder, p, clean(name)});
  return static_cast<int>(pos);
}

bool closeTab(std::vector<TabEntry>& tabs, size_t i) {
  if (i >= tabs.size() || tabs.size() <= 1) return false;
  tabs.erase(tabs.begin() + static_cast<long>(i));
  return true;
}

bool moveTab(std::vector<TabEntry>& tabs, size_t from, size_t to) {
  if (tabs.empty() || from >= tabs.size()) return false;
  to = std::min(to, tabs.size() - 1);
  if (from == to) return false;
  TabEntry t = tabs[from];
  tabs.erase(tabs.begin() + static_cast<long>(from));
  tabs.insert(tabs.begin() + static_cast<long>(to), t);
  return true;
}

bool renameTab(std::vector<TabEntry>& tabs, size_t i, const std::string& name) {
  if (i >= tabs.size() || tabs[i].kind != TabKind::Folder) return false;
  std::string n = clean(name);
  const size_t b = n.find_first_not_of(' '), e = n.find_last_not_of(' ');
  n = b == std::string::npos ? std::string() : n.substr(b, e - b + 1);
  if (n == folderTabLabel(TabEntry{TabKind::Folder, tabs[i].path, {}})) n.clear();  // the folder's own name: follows the folder
  if (n == tabs[i].name) return false;
  tabs[i].name = n;
  return true;
}

std::vector<TabKind> closedBuiltIns(const std::vector<TabEntry>& tabs) {
  std::vector<TabKind> v;
  for (TabKind k : kBuiltIn)
    if (findTab(tabs, k) < 0) v.push_back(k);
  return v;
}

int reopenBuiltIn(std::vector<TabEntry>& tabs, TabKind k) {
  if (k == TabKind::Folder) return -1;
  const int have = findTab(tabs, k);
  if (have >= 0) return have;
  // After the last built-in that comes before it in the usual order (else before the first built-in
  // that comes after it, else at the start).
  const int order = builtInOrder(k);
  size_t pos = 0;
  bool placed = false;
  for (size_t i = tabs.size(); i-- > 0 && !placed;) {
    const int o = builtInOrder(tabs[i].kind);
    if (o >= 0 && o < order) pos = i + 1, placed = true;
  }
  if (!placed)
    for (size_t i = 0; i < tabs.size() && !placed; ++i)
      if (builtInOrder(tabs[i].kind) > order) pos = i, placed = true;
  tabs.insert(tabs.begin() + static_cast<long>(pos), TabEntry{k, {}, {}});
  return static_cast<int>(pos);
}

size_t shownAfterClose(size_t shown, size_t closed, size_t countAfter) {
  if (countAfter == 0) return 0;
  if (shown > closed) return shown - 1;
  if (shown < closed) return shown;
  return std::min(closed, countAfter - 1);
}

std::string folderTabLabel(const TabEntry& t) {
  if (!t.name.empty()) return t.name;
  const std::string p = tidyPath(t.path);
  const size_t s = p.find_last_of("/\\");
  if (s == std::string::npos) return p;
  if (s + 1 >= p.size()) return p;  // the root
  return p.substr(s + 1);
}

std::vector<std::string> folderTabPaths(const std::vector<TabEntry>& tabs) {
  std::vector<std::string> v;
  for (const TabEntry& t : tabs)
    if (t.kind == TabKind::Folder) v.push_back(t.path);
  return v;
}

}  // namespace gl::browse

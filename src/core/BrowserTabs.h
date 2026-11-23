// Gridline — the browser's tabs (GitHub #74): the built-in tabs (All, Project, Plugins, Sounds,
// Starred) and folders the user adds as tabs of their own. The user may add, close, rename and
// reorder them; the list is saved per user in <config>/browser-tabs.txt. Pure logic (no UI): the
// browser window (app/Browser.cpp) draws the strip, the Clips and Smooth layouts list the folder
// tabs among their places.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace gl::browse {

enum class TabKind { All, Project, Plugins, Sounds, Starred, Folder };
constexpr int kBuiltInTabs = 5;  // All .. Starred

struct TabEntry {
  TabKind kind = TabKind::All;
  std::string path;  // Folder: the folder shown
  std::string name;  // Folder: the name given by the user ("" = the folder's own name)
  bool operator==(const TabEntry& o) const { return kind == o.kind && path == o.path && name == o.name; }
};

// The tabs a new user starts with: the five built-in tabs in their usual order.
std::vector<TabEntry> defaultTabs();
// Whether `tabs` is the default set (nothing to save).
bool isDefault(const std::vector<TabEntry>& tabs);

// The saved form: a first line "gridline-browser-tabs 1", then one tab per line ("all", "project",
// "plugins", "sounds", "starred", or "folder<TAB>path<TAB>name"). Unknown lines, repeats and folder
// lines without a path are skipped; a text without a single usable tab gives the defaults.
std::string tabsToText(const std::vector<TabEntry>& tabs);
std::vector<TabEntry> tabsFromText(const std::string& text);

// Built-in tab ids as saved ("all", "project", ...); "" for Folder.
const char* tabId(TabKind k);

// Adds a folder tab at the end (or at `at`), unless that folder already has a tab. Returns the index
// of the folder's tab (the new one or the one already there), or -1 for an empty path.
int addFolderTab(std::vector<TabEntry>& tabs, const std::string& path, const std::string& name = {}, int at = -1);
// Closes tab `i`. The last tab left is never closed (returns false).
bool closeTab(std::vector<TabEntry>& tabs, size_t i);
// Moves tab `from` to index `to` (both clamped). Returns false when nothing moved.
bool moveTab(std::vector<TabEntry>& tabs, size_t from, size_t to);
// Renames a folder tab ("" gives it the folder's own name again). Built-in tabs keep their names.
bool renameTab(std::vector<TabEntry>& tabs, size_t i, const std::string& name);
// The built-in tabs not in `tabs`, in their usual order.
std::vector<TabKind> closedBuiltIns(const std::vector<TabEntry>& tabs);
// Opens a closed built-in tab again, where it stands among the built-ins in their usual order.
// Returns its index, or the index it already had.
int reopenBuiltIn(std::vector<TabEntry>& tabs, TabKind k);
// The first tab of kind `k` (for a folder: showing `path`), or -1.
int findTab(const std::vector<TabEntry>& tabs, TabKind k, const std::string& path = {});
// After tab `closed` was closed: the index to show next (the tab that took its place, else the one
// before it), given the tab shown before (`shown`).
size_t shownAfterClose(size_t shown, size_t closed, size_t countAfter);
// A folder tab's label: its name, else the folder's last part ("/" for the root).
std::string folderTabLabel(const TabEntry& t);
// The folders that have tabs (for the content index and the browser's file operations).
std::vector<std::string> folderTabPaths(const std::vector<TabEntry>& tabs);

}  // namespace gl::browse

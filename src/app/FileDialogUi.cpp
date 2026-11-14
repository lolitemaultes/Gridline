// Gridline: the Open / Save dialog. See FileDialogUi.h.
#include "FileDialogUi.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/ChromeOs.h"
#include "../core/FileDialogModel.h"
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../core/RemoteFs.h"
#include "../engine/ProjectLoad.h"
#include "../engine/Sample.h"
#include "App.h"
#include "DevSignal.h"
#include "AudioRecordUi.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::filedlgui {

using theme::pal;
namespace fs = std::filesystem;
using filedlg::Entry;
using filedlg::SortKey;

namespace {

#if defined(_WIN32)
constexpr bool kWindows = true;
constexpr char kSep = '\\';
#else
constexpr bool kWindows = false;
constexpr char kSep = '/';
#endif

constexpr float kRowH = 24.0f;
constexpr float kSideRowH = 24.0f;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool matches(const std::string& name, const std::vector<std::string>& exts) {
  const std::string l = lower(name);
  for (const auto& e : exts)
    if (l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0) return true;
  return false;
}

// Save: the extension of `name` that one of `exts` matches (the longest), or "".
std::string savedExt(const std::string& name, const std::vector<std::string>& exts) {
  const std::string l = lower(name);
  std::string best;
  for (const auto& e : exts)
    if (e.size() > best.size() && l.size() > e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0) best = e;
  return best;
}

// Save: the file name written, with the chosen type's extension (the caller's nameForType, else
// the first accepted extension when the name has none of them).
std::string savedName(const FileDialogState& fd) {
  if (!fd.save || fd.name.empty() || fd.exts.empty() || (fd.exts.size() == 1 && fd.exts[0] == "/")) return fd.name;
  if (fd.nameForType) return fd.nameForType(fd.name, fd.typeIndex);
  return savedExt(fd.name, fd.exts).empty() ? fd.name + fd.exts[0] : fd.name;
}

// A type picked in the type list: its extensions; a Save name takes the new type's extension.
void setType(FileDialogState& fd, int i) {
  if (fd.types.empty()) return;
  i = std::clamp(i, 0, static_cast<int>(fd.types.size()) - 1);
  const std::vector<std::string> old = fd.exts;
  fd.typeIndex = i;
  fd.exts = fd.types[static_cast<size_t>(i)].exts;
  if (!fd.save || fd.name.empty()) return;
  if (fd.nameForType) {
    fd.name = fd.nameForType(fd.name, fd.typeIndex);
  } else if (!fd.exts.empty() && savedExt(fd.name, fd.exts).empty()) {
    const std::string was = savedExt(fd.name, old);
    fd.name = fd.name.substr(0, fd.name.size() - was.size()) + fd.exts[0];
  }
}

bool remote(const std::string& d) { return remotefs::isRemote(d); }

// A folder that can be shown: a local folder that exists, or any folder on a web or network drive
// (those are only ever looked at in the background).
bool usableDir(const std::string& d) {
  if (d.empty()) return false;
  if (remote(d)) return true;
  std::error_code ec;
  return fs::is_directory(fs::u8path(d), ec);
}

std::string normal(const std::string& d) {
  if (d.empty()) return d;
  std::string s = fs::u8path(d).lexically_normal().u8string();
  while (s.size() > 1 && (s.back() == '/' || (kWindows && s.back() == '\\')) && !(kWindows && s.size() == 3 && s[1] == ':')) s.pop_back();
  return s;
}

bool samePath(const std::string& x, const std::string& y) { return !x.empty() && normal(x) == normal(y); }

std::string parentOf(const std::string& d) {
  const fs::path p = fs::u8path(normal(d));
  const fs::path up = p.parent_path();
  return up.empty() || up == p ? d : up.u8string();
}

std::string home() { return platform::homeDir(); }

struct SideRow {
  std::string name, path, sub;
  ui::Icon icon = ui::Icon::Folder;
  int section = 0;  // 0 drives, 1 places, 2 favourites, 3 recent
};

struct Layout {
  ImVec2 dlgMin, dlgMax, barMin, barMax, sideMin, sideMax, listMin, listMax, footMin, footMax;
};

struct State {
  // Kept for the whole run.
  filedlg::VolumeWatch volumes;
  std::string previewing;  // the audio file playing from the dialog
  // Per opening.
  std::string purpose;
  std::vector<std::string> back, fwd;
  std::string selected;  // row name
  int sortKey = 0;
  bool sortDesc = false, hidden = false;
  char filter[256] = {0};
  bool editingPath = false, focusPath = false;
  char pathBuf[2048] = {0};
  bool focusName = false;
  int selectName = 0;  // frames the name box selects all its text when it takes the focus
  std::string boxText, boxExt;  // the name box and its faint extension, last frame (dev report)
  bool scrollToSel = false, scrollTop = false;
  std::string typeahead;
  double typeaheadAt = -10;
  std::vector<std::string> favourites, recent;
  std::vector<filedlg::Place> places;
  // Listing.
  std::string listDir;
  unsigned listFlags = 0;
  std::shared_ptr<const remotefs::Listing> cur, asyncSeen;
  bool listPending = false;
  double listWaited = 0;
  // Rows, rebuilt when what they depend on changes.
  // Bumped whenever `cur` changes. (Not the listing's address: a new listing can be allocated
  // where the last one was, and the rows of the folder before would stay.)
  uint64_t curGen = 1, rowsGen = 0;
  std::string rowsKey;
  std::vector<Entry> rows;
  // Last frame (dev report).
  std::vector<SideRow> side;
  Layout lay{};
  bool shown = false;
  float lastW = 0, lastH = 0;
  // Dev check: commands applied in the next frame the dialog draws.
  std::vector<std::vector<std::string>> pending;
};

State& S() {
  static State s;
  return s;
}

ui::Icon volumeIcon(filedlg::VolumeKind k) {
  switch (k) {
    case filedlg::VolumeKind::Removable: return ui::Icon::DriveUsb;
    case filedlg::VolumeKind::Network: return ui::Icon::DriveNetwork;
    case filedlg::VolumeKind::Optical: return ui::Icon::Disc;
    case filedlg::VolumeKind::System:
    case filedlg::VolumeKind::Internal: break;
  }
  return ui::Icon::Drive;
}

ui::Icon placeIcon(int i) {
  switch (i) {
    case filedlg::kIconHome: return ui::Icon::Home;
    case filedlg::kIconDesktop: return ui::Icon::Monitor;
    case filedlg::kIconDocuments: return ui::Icon::File;
    case filedlg::kIconMusic: return ui::Icon::Song;
    case filedlg::kIconDownloads: return ui::Icon::Download;
    default: break;
  }
  return ui::Icon::Folder;
}

ui::Icon fileIcon(filedlg::FileKind k) {
  switch (k) {
    case filedlg::FileKind::Folder: return ui::Icon::Folder;
    case filedlg::FileKind::Project: return ui::Icon::ProjectFile;
    case filedlg::FileKind::Audio: return ui::Icon::Wave;
    case filedlg::FileKind::Midi: return ui::Icon::CatMidi;
    case filedlg::FileKind::Preset: return ui::Icon::Mixer;
    case filedlg::FileKind::Other: break;
  }
  return ui::Icon::File;
}

std::string folderLabel(const std::string& p) {
  const std::string n = fs::u8path(normal(p)).filename().u8string();
  return n.empty() ? p : n;
}

void saveSettings(App& a) { a.settings.save(Settings::defaultPath()); }

void go(App& a, State& st, const std::string& dir) {
  FileDialogState& fd = a.fileDialog;
  if (dir.empty() || samePath(dir, fd.dir)) return;
  st.back.push_back(fd.dir);
  if (st.back.size() > 64) st.back.erase(st.back.begin());
  st.fwd.clear();
  fd.dir = dir;
  st.selected.clear();
  st.scrollTop = true;
  st.editingPath = false;
}

void goBack(App& a, State& st) {
  if (st.back.empty()) return;
  st.fwd.push_back(a.fileDialog.dir);
  const std::string prev = a.fileDialog.dir;
  a.fileDialog.dir = st.back.back();
  st.back.pop_back();
  st.selected.clear();
  if (samePath(parentOf(prev), a.fileDialog.dir)) {  // the folder we came out of stays in view
    st.selected = folderLabel(prev);
    st.scrollToSel = true;
  } else {
    st.scrollTop = true;
  }
}

void goForward(App& a, State& st) {
  if (st.fwd.empty()) return;
  st.back.push_back(a.fileDialog.dir);
  a.fileDialog.dir = st.fwd.back();
  st.fwd.pop_back();
  st.selected.clear();
  st.scrollTop = true;
}

void goUp(App& a, State& st) {
  const std::string cur = a.fileDialog.dir, up = parentOf(cur);
  if (samePath(up, cur)) return;
  go(a, st, up);
  st.selected = folderLabel(cur);
  st.scrollToSel = true;
}

std::string userDirsText() {
#if defined(__APPLE__) || defined(_WIN32)
  return "";
#else
  std::string cfg;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) cfg = x;
  else cfg = home() + "/.config";
  std::ifstream in(fs::u8path(cfg) / "user-dirs.dirs");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
#endif
}

// Everything the dialog needs when it opens: the remembered folder, size, sort, places.
void opening(App& a, State& st) {
  FileDialogState& fd = a.fileDialog;
  st.purpose = fd.title;
  st.back.clear();
  st.fwd.clear();
  st.selected.clear();
  st.filter[0] = 0;
  st.editingPath = false;
  st.typeahead.clear();
  st.cur.reset();
  st.asyncSeen.reset();
  st.listDir.clear();
  st.rowsGen = 0;
  st.rowsKey.clear();
  st.sortKey = std::clamp(static_cast<int>(a.settings.getNumber("files.dialogSort")), 0, 3);
  st.sortDesc = a.settings.getBool("files.dialogSortDesc");
  st.hidden = a.settings.getBool("files.dialogHidden");
  st.favourites = filedlg::decodeList(a.settings.getString("files.dialogFavourites"));
  st.recent = filedlg::decodeList(a.settings.getString("files.dialogRecent"));
  // The last folder this dialog was used in, unless the caller starts somewhere of its own (the
  // song's folder, the current sample's folder).
  if (fd.dir.empty() || samePath(fd.dir, home())) {
    const std::string last = filedlg::lastFolder(a.settings.getString("files.dialogLast"), st.purpose);
    if (!last.empty() && usableDir(last)) fd.dir = last;
  }
  // Places: only named here; a folder is read when the user goes there.
  st.places.clear();
  for (auto& p : filedlg::standardPlaces(home(), userDirsText())) {
    std::error_code ec;
    if (p.icon == filedlg::kIconHome || fs::is_directory(fs::u8path(p.path), ec)) st.places.push_back(std::move(p));
  }
  const auto addOwn = [&](const char* name, const std::string& path) {
    if (path.empty()) return;
    for (const auto& p : st.places)
      if (samePath(p.path, path)) return;
    std::error_code ec;
    if (remote(path) || fs::is_directory(fs::u8path(path), ec)) st.places.push_back({name, path, filedlg::kIconFolder});
  };
  if (!a.projectPath.empty()) addOwn(i18n::N_("Project folder"), fs::u8path(a.projectPath).parent_path().u8string());
  addOwn(i18n::N_("Recorded"), recordedDir(a));
  addOwn(i18n::N_("Rendered"), renderedDir(a));
  // K-75: in a Chromebook's (or the Android terminal's) Linux environment, the folders shared from the host.
  const std::string shared = chromeos::sharedFolder(chromeos::current());
  if (!shared.empty()) st.places.push_back({i18n::N_("Shared folders"), shared, filedlg::kIconFolder});
  st.focusName = fd.save;
  // A field still active from the last time the dialog was open (it closed while typing, or by
  // Enter) would take the focus again as it was, with the old text and no selection: the name box
  // would add to the old name instead of replacing it.
  if (ImGui::GetActiveID() != 0) ImGui::ClearActiveID();
  st.volumes.poll(0.0);  // a fresh look at the drives
}

// The dialog was accepted in `dir`: remembered for this dialog and in the recent folders.
void remember(App& a, State& st, const std::string& dir) {
  if (dir.empty()) return;
  a.settings.set("files.dialogLast", filedlg::setLastFolder(a.settings.getString("files.dialogLast"), st.purpose, dir));
  filedlg::pushRecent(st.recent, dir);
  a.settings.set("files.dialogRecent", filedlg::encodeList(st.recent));
  saveSettings(a);
}

void setSort(App& a, State& st, int key, bool desc) {
  st.sortKey = std::clamp(key, 0, 3);
  st.sortDesc = desc;
  a.settings.set("files.dialogSort", st.sortKey);
  a.settings.set("files.dialogSortDesc", st.sortDesc);
}

void setHidden(App& a, State& st, bool on) {
  st.hidden = on;
  a.settings.set("files.dialogHidden", on);
}

void pin(App& a, State& st, bool on) {
  if (filedlg::setPinned(st.favourites, normal(a.fileDialog.dir), on)) {
    a.settings.set("files.dialogFavourites", filedlg::encodeList(st.favourites));
    saveSettings(a);
  }
}

// The folder's listing: a local one at once and again every two seconds in the background, one
// on a web or network drive only ever in the background.
void refreshListing(App& a, State& st) {
  const std::string& dir = a.fileDialog.dir;
  const unsigned flags = remotefs::kListDetails | (st.hidden ? remotefs::kListHidden : 0u);
  const bool far = remote(dir);
  const bool changed = dir != st.listDir || flags != st.listFlags;
  remotefs::DirState ds = remotefs::listAsync(dir, far ? 15.0 : 2.0, flags);
  if (changed) {
    st.listDir = dir;
    st.listFlags = flags;
    st.cur.reset();
    ++st.curGen;
    st.asyncSeen = ds.listing;  // whatever was there before is older than this visit
    if (!far) st.cur = std::make_shared<const remotefs::Listing>(remotefs::listNow(dir, flags));
    else if (ds.listing && !ds.pending) st.cur = ds.listing;
  }
  if (ds.listing && ds.listing != st.asyncSeen) {
    st.asyncSeen = ds.listing;
    st.cur = ds.listing;
    ++st.curGen;
  }
  st.listPending = ds.pending && !st.cur;
  if (st.cur) devsig::reach("filedialog:" + normal(dir));  // dev scripts wait for a folder's rows with "waitfor"
  st.listWaited = ds.waited;
}

void rebuildRows(App& a, State& st, bool folderMode) {
  const FileDialogState& fd = a.fileDialog;
  std::string key = std::to_string(st.sortKey) + (st.sortDesc ? "d" : "a") + (st.hidden ? "h" : "") + (folderMode ? "f" : "") + '\n' + st.filter + '\n';
  for (const auto& e : fd.exts) key += e + ' ';
  if (st.rowsGen == st.curGen && key == st.rowsKey) return;
  st.rowsGen = st.curGen;
  st.rowsKey = key;
  st.rows.clear();
  if (!st.cur) return;
  const std::string f = lower(st.filter);
  for (const auto& e : st.cur->entries) {
    if (e.hidden && !st.hidden) continue;
    if (!e.isDir && (folderMode || !matches(e.name, fd.exts))) continue;
    if (!f.empty() && lower(e.name).find(f) == std::string::npos) continue;
    Entry r;
    r.name = e.name;
    r.isDir = e.isDir;
    r.hidden = e.hidden;
    r.size = e.size;
    r.mtime = e.mtime;
    r.kind = filedlg::kindOf(e.name, e.isDir);
    st.rows.push_back(std::move(r));
  }
  filedlg::sortEntries(st.rows, static_cast<SortKey>(st.sortKey), st.sortDesc);
}

int selectedIndex(const State& st) {
  for (size_t i = 0; i < st.rows.size(); ++i)
    if (st.rows[i].name == st.selected) return static_cast<int>(i);
  return -1;
}

std::string pathOf(const App& a, const std::string& name) { return (fs::u8path(a.fileDialog.dir) / fs::u8path(name)).u8string(); }

void togglePreview(App& a, State& st, const std::string& file) {
  if (st.previewing == file) {
    a.stopPreview();
    st.previewing.clear();
    return;
  }
  if (remote(file)) a.previewWhenReady(file);
  else a.previewSample(file);
  st.previewing = file;
}

// A row picked by a click or the keyboard: a file sets the name; a sound plays (stops when another is picked).
void selectRow(App& a, State& st, const Entry& r, bool preview) {
  st.selected = r.name;
  if (r.isDir) return;
  a.fileDialog.name = r.name;
  const std::string full = pathOf(a, r.name);
  if (preview && r.kind == filedlg::FileKind::Audio && st.previewing != full) togglePreview(a, st, full);
}

float textW(const App& a, float size, const char* t) { return a.fontRegular ? a.fontRegular->CalcTextSizeA(size, FLT_MAX, 0.0f, t).x : ImGui::CalcTextSize(t).x; }

std::string timeText(int64_t t) {
  if (t <= 0) return "";
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

// The names in a typed path's folder, for Tab (folders end in '/').
std::vector<std::string> namesIn(const std::string& folder, bool hidden) {
  std::vector<std::string> out;
  if (folder.empty()) return out;
  std::shared_ptr<const remotefs::Listing> l;
  const unsigned flags = hidden ? remotefs::kListHidden : 0u;
  if (remote(folder)) l = remotefs::listAsync(folder, 15.0, flags).listing;  // whatever is known; never waits
  else l = std::make_shared<const remotefs::Listing>(remotefs::listNow(folder, flags));
  if (!l) return out;
  for (const auto& e : l->entries) out.push_back(e.isDir ? e.name + '/' : e.name);
  return out;
}

std::string expandTyped(std::string t) {
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (!t.empty() && t[0] == '~' && (t.size() == 1 || t[1] == '/' || t[1] == '\\')) t = home() + t.substr(1);
  return t;
}

int completeCallback(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag != ImGuiInputTextFlags_CallbackCompletion) return 0;
  const std::string typed(d->Buf, static_cast<size_t>(d->BufTextLen));
  const std::string full = expandTyped(typed);
  std::string folder;
  filedlg::splitTyped(full, kWindows, &folder, nullptr);
  const std::string done = filedlg::completePath(full, namesIn(folder, S().hidden), kWindows);
  if (done != full) {
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, done.c_str());
  }
  return 0;
}

// Enter in the path bar: a folder opens; a file is chosen (Open) or named (Save).
// Returns the file to accept, or "".
std::string typedPath(App& a, State& st) {
  FileDialogState& fd = a.fileDialog;
  const bool folderMode = fd.exts.size() == 1 && fd.exts[0] == "/";
  std::string t = expandTyped(st.pathBuf);
  if (t.empty()) return "";
  if (!fs::u8path(t).is_absolute()) t = (fs::u8path(fd.dir) / fs::u8path(t)).u8string();
  t = normal(t);
  if (remote(t)) {
    const remotefs::Probe p = remotefs::probe(t, 5.0);  // never waits: not known yet counts as a folder (its listing then says)
    if (!p.known || p.isDir) {
      go(a, st, t);
      return "";
    }
    if (p.exists && !folderMode) {
      go(a, st, parentOf(t));
      fd.name = fs::u8path(t).filename().u8string();
      return fd.save ? "" : t;
    }
  } else {
    std::error_code ec;
    if (fs::is_directory(fs::u8path(t), ec)) {
      go(a, st, t);
      return "";
    }
    if (fs::exists(fs::u8path(t), ec) && !folderMode) {
      go(a, st, parentOf(t));
      fd.name = fs::u8path(t).filename().u8string();
      st.selected = fd.name;
      st.scrollToSel = true;
      return fd.save ? "" : t;
    }
    if (fd.save && !folderMode && fs::is_directory(fs::u8path(parentOf(t)), ec)) {  // a new file's name
      go(a, st, parentOf(t));
      fd.name = fs::u8path(t).filename().u8string();
      return "";
    }
  }
  a.notify("There is no folder or file by that name", true, t);
  st.editingPath = true;
  st.focusPath = true;
  return "";
}

// ---- drawing ----

void caption(App& a, const char* text) {
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  theme::textEllipsis(dl, a.fontBold, theme::kFontLabel, ImVec2(p.x + theme::kS2, p.y + 2), ImGui::GetContentRegionAvail().x - theme::kS3,
                      pal().textFaint, i18n::tr(text));
  ImGui::Dummy(ImVec2(0, theme::kFontLabel + 6));
}

// A sidebar row; true when clicked. `remove` gets an x on hover (favourites).
bool sideRow(App& a, State& st, const SideRow& r, bool current, bool* remove) {
  const auto& P = pal();
  ImGui::PushID(r.path.c_str());
  ImGui::PushID(r.section);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const bool clicked = ImGui::InvisibleButton("##s", ImVec2(w, kSideRowH));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (current || hov)
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + kSideRowH), current ? theme::withAlpha(P.accent, 0.28f) : P.surface800, theme::kRadiusSm);
  ui::drawIcon(dl, r.icon, ImVec2(q.x + 14, q.y + kSideRowH * 0.5f), 14, current ? P.text : P.textDim);
  float subW = 0;
  if (!r.sub.empty() && !(remove && hov)) {
    subW = textW(a, theme::kFontLabel, r.sub.c_str());
    if (subW > w * 0.4f) subW = 0;  // too narrow: the name keeps the room
    else theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(q.x + w - subW - 6, q.y + 6), subW + 2, P.textFaint, r.sub.c_str());
  }
  bool removed = false;
  if (remove && hov) {
    const ImVec2 c(q.x + w - 12, q.y + kSideRowH * 0.5f);
    const bool overX = ImGui::IsMouseHoveringRect(ImVec2(c.x - 9, c.y - 9), ImVec2(c.x + 9, c.y + 9));
    ui::drawIcon(dl, ui::Icon::Close, c, 12, overX ? P.text : P.textFaint);
    subW = 20;
    if (clicked && overX) removed = true;
  }
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 28, q.y + 5), w - 34 - (subW > 0 ? subW + 8 : 0), current ? P.text : P.text,
                      r.name.c_str());
  if (hov) ui::Tooltip(r.path.c_str());
  ImGui::PopID();
  ImGui::PopID();
  if (remove) *remove = removed;
  (void)st;
  return clicked && !removed;
}

void sidebar(App& a, State& st, ImVec2 size) {
  const auto& P = pal();
  FileDialogState& fd = a.fileDialog;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, theme::kS1));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##places", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  const ImVec2 wmin = ImGui::GetWindowPos();
  st.lay.sideMin = wmin;
  st.lay.sideMax = ImVec2(wmin.x + ImGui::GetWindowSize().x, wmin.y + ImGui::GetWindowSize().y);
  st.side.clear();
  // The row of the folder shown is lit (one row: the first that is exactly this folder).
  bool lit = false;
  std::string go_;
  auto list = [&](int section, std::vector<SideRow>& rows, bool removable) {
    size_t best = rows.size();
    for (size_t i = 0; i < rows.size() && !lit; ++i)
      if (samePath(rows[i].path, fd.dir)) best = i, lit = true;
    for (size_t i = 0; i < rows.size(); ++i) {
      rows[i].section = section;
      bool rm = false;
      if (sideRow(a, st, rows[i], i == best, removable ? &rm : nullptr)) go_ = rows[i].path;
      if (rm) {
        filedlg::setPinned(st.favourites, rows[i].path, false);
        a.settings.set("files.dialogFavourites", filedlg::encodeList(st.favourites));
        saveSettings(a);
      }
      st.side.push_back(rows[i]);
    }
  };
  // Drives and volumes.
  caption(a, i18n::N_("Drives"));
  {
    auto vols = st.volumes.poll(2.0);  // plugged in or taken out: seen within a couple of seconds
    std::vector<SideRow> rows;
    if (vols)
      for (const auto& v : *vols) {
        SideRow r;
        r.name = v.kind == filedlg::VolumeKind::System ? i18n::tr(v.name.c_str()) : v.name;
        r.path = v.path;
        r.icon = volumeIcon(v.kind);
        if (v.freeBytes >= 0) {
          char buf[48];
          std::snprintf(buf, sizeof buf, i18n::tr("%s free"), filedlg::sizeText(v.freeBytes).c_str());
          r.sub = buf;
        }
        rows.push_back(std::move(r));
      }
    if (!vols) {
      ImGui::Dummy(ImVec2(0, 2));
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS2);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Looking for drives"));
    }
    list(0, rows, false);
  }
  caption(a, i18n::N_("Places"));
  {
    std::vector<SideRow> rows;
    for (const auto& p : st.places) rows.push_back({i18n::tr(p.name.c_str()), p.path, "", placeIcon(p.icon), 1});
    list(1, rows, false);
  }
  caption(a, i18n::N_("Favourites"));
  {
    std::vector<SideRow> rows;
    for (const auto& f : st.favourites) rows.push_back({folderLabel(f), f, "", ui::Icon::Star, 2});
    if (rows.empty()) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS2);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - theme::kS2);
      ImGui::PushFont(nullptr, theme::kFontCaption);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Pin a folder with the star above the list"));
      ImGui::PopFont();
      ImGui::PopTextWrapPos();
    }
    list(2, rows, true);
  }
  if (!st.recent.empty()) {
    caption(a, i18n::N_("Recent folders"));
    std::vector<SideRow> rows;
    for (const auto& r : st.recent) rows.push_back({folderLabel(r), r, "", ui::Icon::Clock, 3});
    list(3, rows, false);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (!go_.empty()) {
    // Shared folders (K-75): until one is shared, say how.
    const std::string shared = chromeos::sharedFolder(chromeos::current());
    if (!shared.empty() && samePath(go_, shared) && !usableDir(shared)) a.notify(chromeos::shareHint(chromeos::current()));
    else if (usableDir(go_)) go(a, st, go_);
    else a.notify("This folder is not there any more", true, go_);
  }
}

// The breadcrumb bar (or the typed path while editing). Returns a file to accept, or "".
std::string pathBar(App& a, State& st, float width) {
  const auto& P = pal();
  FileDialogState& fd = a.fileDialog;
  const float fh = ImGui::GetFrameHeight();
  std::string accept;
  if (ImGui::IsKeyPressed(ImGuiKey_L) && ImGui::GetIO().KeyCtrl && !st.editingPath) {
    st.editingPath = true;
    st.focusPath = true;
    std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s", fd.dir.c_str());
  }
  if (st.editingPath) {
    if (st.focusPath) {
      ImGui::SetKeyboardFocusHere();
      st.focusPath = false;
    }
    ImGui::SetNextItemWidth(width);
    const bool enter = ImGui::InputTextWithHint("##path", i18n::tr("Type a folder or file, Tab completes"), st.pathBuf, sizeof st.pathBuf,
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion, completeCallback);
    ui::FocusRing();
    if (enter) {
      st.editingPath = false;
      accept = typedPath(a, st);
    } else if (ImGui::IsItemDeactivated()) {  // Escape, or a click elsewhere
      st.editingPath = false;
    }
    return accept;
  }
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + fh), P.field, theme::kRadius);
  const std::vector<filedlg::Crumb> crumbs = filedlg::splitBreadcrumb(fd.dir, kWindows);
  // Parts from the right that fit; the rest folds into "...".
  const float pad = 8, chev = 14;
  std::vector<float> widths;
  for (const auto& c : crumbs) widths.push_back(ImGui::CalcTextSize(c.label.c_str()).x + pad * 2);
  const float ellW = ImGui::CalcTextSize("...").x + pad * 2;
  const float room = width - 28;  // the free end of the bar opens typing
  size_t first = 0;
  {
    float used = 0;
    size_t i = crumbs.size();
    while (i > 0) {
      const float need = widths[i - 1] + (i < crumbs.size() ? chev : 0);
      if (used + need + (i > 1 ? ellW + chev : 0) > room && i < crumbs.size()) break;
      used += need;
      --i;
    }
    first = i;
  }
  float x = p0.x + 2;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  auto crumbButton = [&](const char* id, const std::string& label, float w, bool last) {
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    const bool c = ImGui::InvisibleButton(id, ImVec2(w, fh));
    const bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(ImVec2(x + 1, p0.y + 2), ImVec2(x + w - 1, p0.y + fh - 2), P.surface700, theme::kRadiusSm);
    const float lw = std::min(w - pad * 2, ImGui::CalcTextSize(label.c_str()).x);
    theme::textEllipsis(dl, last ? a.fontBold : a.fontRegular, ImGui::GetFontSize(), ImVec2(x + pad, p0.y + (fh - ImGui::GetFontSize()) * 0.5f), lw + 1,
                        last ? P.text : P.textDim, label.c_str());
    x += w;
    return c;
  };
  if (first > 0) {
    if (crumbButton("##ell", "...", ellW, false)) {
      st.editingPath = true;
      st.focusPath = true;
      std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s", fd.dir.c_str());
    }
    ui::Tooltip(fd.dir.c_str());
    ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(x + chev * 0.5f, p0.y + fh * 0.5f), 10, P.textFaint);
    x += chev;
  }
  std::string jump;
  for (size_t i = first; i < crumbs.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    const bool last = i + 1 == crumbs.size();
    const float w = std::min(widths[i], p0.x + width - 28 - x);
    if (w > pad * 2 && crumbButton("##c", crumbs[i].label, w, last) && !last) jump = crumbs[i].path;
    ImGui::PopID();
    if (!last) {
      ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(x + chev * 0.5f, p0.y + fh * 0.5f), 10, P.textFaint);
      x += chev;
    }
  }
  // The rest of the bar: click to type a path.
  const float restW = p0.x + width - x;
  if (restW > 4) {
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    if (ImGui::InvisibleButton("##type", ImVec2(restW, fh))) {
      st.editingPath = true;
      st.focusPath = true;
      std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s%s", fd.dir.c_str(), fd.dir.empty() || fd.dir.back() == kSep || fd.dir.back() == '/' ? "" : std::string(1, kSep).c_str());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
      ui::Tooltip(i18n::N_("Click to type a path"));
    }
    ui::drawIcon(dl, ui::Icon::Draw, ImVec2(p0.x + width - 14, p0.y + fh * 0.5f), 12, P.textFaint);
  }
  ImGui::PopStyleVar();
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(width, fh));
  if (!jump.empty()) go(a, st, jump);
  return accept;
}

}  // namespace

void draw(App& a) {
  FileDialogState& fd = a.fileDialog;
  State& st = S();
  const auto& P = pal();
  if (fd.open) {
    ImGui::OpenPopup("##filedialog");
    fd.open = false;
    opening(a, st);
  }
  {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float sw = static_cast<float>(a.settings.getNumber("files.dialogW")), sh = static_cast<float>(a.settings.getNumber("files.dialogH"));
    ImVec2 want(sw >= 560 ? sw : 960.0f, sh >= 380 ? sh : 620.0f);
    want.x = std::min(want.x, vp->WorkSize.x - 48);
    want.y = std::min(want.y, vp->WorkSize.y - 48);
    ui::PlaceNextDialog(want, ImGuiCond_Appearing, ImVec2(std::min(560.0f, vp->WorkSize.x - 16), std::min(380.0f, vp->WorkSize.y - 16)));
  }
  bool keep = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool open = ImGui::BeginPopupModal("##filedialog", &keep, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  st.shown = open;
  if (!open) return;
  const bool appearing = ImGui::IsWindowAppearing();
  if (!fd.devAccept.empty() && !appearing) {  // J-167 dev check: as if this file were picked and OK clicked
    const std::string chosen = std::move(fd.devAccept);
    fd.devAccept.clear();
    auto cb = fd.onOk;
    ImGui::CloseCurrentPopup();
    if (cb) cb(chosen);
    ImGui::EndPopup();
    return;
  }
  {
    const ImVec2 ws = ImGui::GetWindowSize();
    if (!appearing && (std::abs(ws.x - st.lastW) > 0.5f || std::abs(ws.y - st.lastH) > 0.5f) && st.lastW > 0) {
      a.settings.set("files.dialogW", static_cast<double>(ws.x));  // the user resized it: kept for next time
      a.settings.set("files.dialogH", static_cast<double>(ws.y));
    }
    st.lastW = ws.x;
    st.lastH = ws.y;
    st.lay.dlgMin = ImGui::GetWindowPos();
    st.lay.dlgMax = ImVec2(st.lay.dlgMin.x + ws.x, st.lay.dlgMin.y + ws.y);
  }
  const bool folderMode = fd.exts.size() == 1 && fd.exts[0] == "/";
  std::string chosen;
  bool okDev = false;

  // Dev commands queued for this frame.
  for (const auto& c : st.pending) {
    const std::string& cmd = c[0];
    const std::string arg = c.size() > 1 ? c[1] : "";
    if (cmd == "go") go(a, st, arg);
    else if (cmd == "back") goBack(a, st);
    else if (cmd == "forward") goForward(a, st);
    else if (cmd == "up") goUp(a, st);
    else if (cmd == "pin") pin(a, st, true);
    else if (cmd == "unpin") pin(a, st, false);
    else if (cmd == "hidden") setHidden(a, st, arg != "off");
    else if (cmd == "filter") std::snprintf(st.filter, sizeof st.filter, "%s", arg.c_str());
    else if (cmd == "accept") okDev = true;
    else if (cmd == "type") setType(fd, std::atoi(arg.c_str()));
    else if (cmd == "sort") {
      const int k = arg == "size" ? 1 : arg == "kind" ? 2 : arg == "modified" ? 3 : 0;
      setSort(a, st, k, c.size() > 2 && c[2] == "desc");
    } else if (cmd == "path") {
      std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s", arg.c_str());
      const std::string f = typedPath(a, st);
      if (!f.empty()) chosen = f;
    } else if (cmd == "complete") {
      const std::string full = expandTyped(arg);
      std::string folder;
      filedlg::splitTyped(full, kWindows, &folder, nullptr);
      std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s", filedlg::completePath(full, namesIn(folder, st.hidden), kWindows).c_str());
      std::printf("filedialog complete: %s\n", st.pathBuf);
      std::fflush(stdout);
    } else if (cmd == "select") {
      for (const auto& r : st.rows)
        if (r.name == arg) selectRow(a, st, r, false);
    } else if (cmd == "place") {
      for (const auto& r : st.side)
        if (r.name == arg) {
          go(a, st, r.path);
          break;
        }
    }
  }
  st.pending.clear();

  // Title.
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(i18n::tr(fd.title.c_str()));
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, theme::kS1));

  // Bar: back, forward, up, the path, the filter, hidden files, pin.
  if (fd.dir.empty() || (!remote(fd.dir) && !usableDir(fd.dir))) fd.dir = home();
  const float fh = ImGui::GetFrameHeight();
  st.lay.barMin = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(st.back.empty());
  if (ui::IconButton("back", ui::Icon::ChevronLeft, false, fh, "Back")) goBack(a, st);
  ImGui::EndDisabled();
  ImGui::SameLine(0, 2);
  ImGui::BeginDisabled(st.fwd.empty());
  if (ui::IconButton("fwd", ui::Icon::Chevron, false, fh, "Forward")) goForward(a, st);
  ImGui::EndDisabled();
  ImGui::SameLine(0, 2);
  if (ui::IconButton("up", ui::Icon::ChevronUp, false, fh, "Parent folder")) goUp(a, st);
  ImGui::SameLine(0, theme::kS2);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float filterW = std::clamp(avail * 0.24f, 120.0f, 200.0f);
  const float barW = std::max(120.0f, avail - filterW - fh * 2 - theme::kS2 * 2 - 2);
  {
    const std::string f = pathBar(a, st, barW);
    if (!f.empty()) chosen = f;
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(filterW);
  ImGui::InputTextWithHint("##filter", i18n::tr("Filter"), st.filter, sizeof st.filter);
  ui::FocusRing();
  if (ImGui::IsItemHovered()) ui::Tooltip(i18n::N_("Show only the names that contain this text"));
  ImGui::SameLine(0, theme::kS2);
  if (ui::IconButton("hidden", st.hidden ? ui::Icon::Eye : ui::Icon::EyeOff, st.hidden, fh, st.hidden ? "Hide hidden files" : "Show hidden files"))
    setHidden(a, st, !st.hidden);
  ImGui::SameLine(0, 2);
  const bool pinned = filedlg::contains(st.favourites, normal(fd.dir));
  if (ui::IconButton("pin", pinned ? ui::Icon::StarFilled : ui::Icon::Star, pinned, fh, pinned ? "Remove this folder from Favourites" : "Pin this folder to Favourites"))
    pin(a, st, !pinned);
  st.lay.barMax = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
  ImGui::Dummy(ImVec2(0, theme::kS1));

  // Body: places on the left, the list on the right; the footer below is reserved exactly.
  const float footer = ui::DialogFooterHeight(2);
  const ImVec2 body(ImGui::GetContentRegionAvail().x, std::max(80.0f, ImGui::GetContentRegionAvail().y - footer));
  const float sideW = std::clamp(body.x * 0.24f, 150.0f, 224.0f);
  sidebar(a, st, ImVec2(sideW, body.y));
  ImGui::SameLine(0, theme::kS2);

  refreshListing(a, st);
  rebuildRows(a, st, folderMode);

  // Keyboard: arrows, Home / End, Enter, Backspace, Alt + arrows, typing to jump.
  const ImGuiIO& io = ImGui::GetIO();
  bool enterOnRow = false;
  if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !appearing) {
    int i = selectedIndex(st);
    const int n = static_cast<int>(st.rows.size());
    int to = i;
    if (io.KeyAlt) {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) goBack(a, st);
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) goForward(a, st);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) goUp(a, st);
    } else if (n > 0) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) to = std::min(n - 1, i + 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) to = i < 0 ? 0 : std::max(0, i - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) to = std::min(n - 1, std::max(0, i) + 12);
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) to = std::max(0, i - 12);
      if (ImGui::IsKeyPressed(ImGuiKey_Home)) to = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_End)) to = n - 1;
    }
    if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Backspace)) goUp(a, st);
    // Typing jumps to the first name that starts with what was typed (within a second).
    if (!io.KeyCtrl && !io.KeySuper && !io.KeyAlt)
      for (int k = 0; k < io.InputQueueCharacters.Size; ++k) {
        const ImWchar ch = io.InputQueueCharacters[k];
        if (ch < 32 || ch == 127) continue;
        if (ch == '/' || ch == '~' || ch == '\\') {  // a path: the bar takes it
          st.editingPath = true;
          st.focusPath = true;
          std::snprintf(st.pathBuf, sizeof st.pathBuf, "%s", ch == '~' ? "~" : std::string(1, static_cast<char>(ch)).c_str());
          break;
        }
        if (a.now - st.typeaheadAt > 1.0) st.typeahead.clear();
        st.typeaheadAt = a.now;
        if (ch < 128) st.typeahead += static_cast<char>(std::tolower(ch));
        const std::string t = st.typeahead;
        for (int r = 0; r < n; ++r)
          if (lower(st.rows[static_cast<size_t>(r)].name).compare(0, t.size(), t) == 0) {
            to = r;
            break;
          }
      }
    if (to != i && to >= 0 && to < n) {
      selectRow(a, st, st.rows[static_cast<size_t>(to)], true);
      st.scrollToSel = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      const int s = selectedIndex(st);
      if (s >= 0 && st.rows[static_cast<size_t>(s)].isDir) {
        go(a, st, pathOf(a, st.rows[static_cast<size_t>(s)].name));
        enterOnRow = true;
      }
    }
  }

  // The list.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##listbox", ImVec2(0, body.y), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  {
    const ImVec2 wp = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
    st.lay.listMin = wp;
    st.lay.listMax = ImVec2(wp.x + wsz.x, wp.y + wsz.y);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float listW = ImGui::GetContentRegionAvail().x;
  // Columns from the right: Modified, Kind, Size; a narrow list drops Kind, then Modified.
  const float modW = listW >= 470 ? 128.0f : 0.0f, kindW = listW >= 600 ? 92.0f : 0.0f, sizeW = 76.0f;
  const float nameW = listW - modW - kindW - sizeW - theme::kS2;
  const float xName = 0, xSize = nameW, xKind = xSize + sizeW + theme::kS2, xMod = xKind + kindW;
  // Header.
  {
    const ImVec2 h0 = ImGui::GetCursorScreenPos();
    const float hh = 24;
    dl->AddLine(ImVec2(h0.x - theme::kS1, h0.y + hh - 0.5f), ImVec2(h0.x + listW + theme::kS1, h0.y + hh - 0.5f), P.line);
    struct Col {
      const char* label;
      float x, w;
      int key;
      bool right;
    };
    const Col cols[] = {{i18n::N_("Name"), xName, nameW, 0, false}, {i18n::N_("Size"), xSize, sizeW, 1, true},
                        {i18n::N_("Kind"), xKind, kindW, 2, false}, {i18n::N_("Modified"), xMod, modW, 3, false}};
    for (const auto& c : cols) {
      if (c.w <= 0) continue;
      ImGui::PushID(c.key);
      ImGui::SetCursorScreenPos(ImVec2(h0.x + c.x, h0.y));
      if (ImGui::InvisibleButton("##h", ImVec2(c.w, hh))) setSort(a, st, c.key, st.sortKey == c.key ? !st.sortDesc : false);
      const bool hov = ImGui::IsItemHovered();
      if (hov) ui::Tooltip(i18n::N_("Sort by this column; click again to reverse"));
      const bool on = st.sortKey == c.key;
      const char* t = i18n::tr(c.label);
      const float tw = textW(a, theme::kFontCaption, t);
      const float arrowW = on ? 12.0f : 0.0f;
      float tx = c.right ? h0.x + c.x + c.w - tw - arrowW - 6 : h0.x + c.x + 6;
      if (c.key == 0) tx += 24;
      theme::textEllipsis(dl, on ? a.fontBold : a.fontRegular, theme::kFontCaption, ImVec2(tx, h0.y + 6), c.w - 12 - arrowW, on || hov ? P.text : P.textDim, t);
      if (on) ui::drawIcon(dl, st.sortDesc ? ui::Icon::ChevronDown : ui::Icon::ChevronUp, ImVec2(std::min(tx + tw + 8, h0.x + c.x + c.w - 6), h0.y + hh * 0.5f), 10, P.hot);
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + hh));
  }
  // Rows.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None);
  ImDrawList* rl = ImGui::GetWindowDrawList();
  const char* noteTitle = nullptr;
  const char* noteBody = "";
  if (remote(fd.dir) && st.listPending) {
    const bool stalled = st.listWaited >= projload::kStallSeconds;
    noteTitle = stalled ? i18n::N_("The drive is not answering") : i18n::N_("Reading this folder");
    noteBody = stalled ? i18n::N_("It may be offline. Go up a folder, or keep waiting") : i18n::N_("On a web drive this can take a moment");
  } else if (st.cur && !st.cur->ok && st.cur->entries.empty()) {
    noteTitle = i18n::N_("This folder cannot be read");
    noteBody = i18n::N_("If it is on a drive, check that the drive is connected and online");
  }
  if (st.scrollTop) {
    ImGui::SetScrollY(0);
    st.scrollTop = false;
  }
  const int selIdx = selectedIndex(st);
  if (st.scrollToSel && selIdx >= 0) {
    const float y = selIdx * kRowH, top = ImGui::GetScrollY(), h = ImGui::GetWindowHeight();
    if (y < top) ImGui::SetScrollY(y);
    else if (y + kRowH > top + h) ImGui::SetScrollY(y + kRowH - h);
    st.scrollToSel = false;
  }
  std::string goInto;
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(st.rows.size()), kRowH);
  while (clip.Step())
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      const Entry& r = st.rows[static_cast<size_t>(i)];
      ImGui::PushID(i);
      const ImVec2 q = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const bool sel = r.name == st.selected || (!r.isDir && st.selected.empty() && fd.name == r.name);
      const bool clicked = ImGui::InvisibleButton("##r", ImVec2(w, kRowH));
      const bool hov = ImGui::IsItemHovered();
      const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
      if (sel || hov) rl->AddRectFilled(q, ImVec2(q.x + w, q.y + kRowH), sel ? theme::withAlpha(P.accent, 0.3f) : P.surface800, theme::kRadiusSm);
      const std::string full = pathOf(a, r.name);
      const bool audio = r.kind == filedlg::FileKind::Audio;
      const bool playing = audio && st.previewing == full;
      const ImVec2 ic(q.x + 14, q.y + kRowH * 0.5f);
      const bool overIcon = audio && ImGui::IsMouseHoveringRect(ImVec2(q.x, q.y), ImVec2(q.x + 26, q.y + kRowH));
      const ImU32 icol = r.isDir ? P.textDim : r.kind == filedlg::FileKind::Project ? P.hot : r.kind == filedlg::FileKind::Other ? P.textFaint : P.textDim;
      if (audio && (playing || (hov && overIcon))) ui::drawIcon(rl, playing ? ui::Icon::Stop : ui::Icon::Play, ic, 12, playing ? P.hot : P.text);
      else ui::drawIcon(rl, fileIcon(r.kind), ic, 14, icol);
      const ImU32 tcol = r.hidden ? P.textDim : P.text;
      theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + xName + 30, q.y + 5), nameW - 36, tcol, r.name.c_str());
      if (!r.isDir && r.size >= 0) {
        const std::string s = filedlg::sizeText(r.size);
        const float tw = textW(a, theme::kFontCaption, s.c_str());
        theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + xSize + sizeW - tw - 6, q.y + 6), sizeW - 6, P.textDim, s.c_str());
      }
      if (kindW > 0)
        theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + xKind + 6, q.y + 6), kindW - 10, P.textDim,
                            i18n::tr(filedlg::kindLabel(r.kind, r.name)));
      if (modW > 0) {
        const std::string t = timeText(r.mtime);
        theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + xMod + 6, q.y + 6), modW - 10, P.textDim, t.c_str());
      }
      if (clicked) {
        if (overIcon) {
          st.selected = r.name;
          fd.name = r.name;
          togglePreview(a, st, full);
        } else {
          selectRow(a, st, r, true);  // a picked sound plays, as before
        }
      }
      if (dbl) {
        if (r.isDir) goInto = full;
        else chosen = full;
      }
      ImGui::PopID();
    }
  clip.End();
  if (st.rows.empty()) {
    const ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    const bool filtered = st.filter[0] != 0;
    ui::EmptyState(ImGui::GetWindowDrawList(), mn, ImVec2(mn.x + sz.x, mn.y + sz.y), noteTitle ? noteTitle : filtered ? i18n::N_("Nothing matches the filter") : "This folder is empty",
                   noteTitle ? noteBody : filtered ? i18n::N_("Clear the filter to see every file") : folderMode ? "Choose it, or open a sub-folder" : "No matching files here");
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  if (!goInto.empty()) go(a, st, goInto);

  // Footer: the name (or the folder chosen), the type list, Cancel and Open / Save.
  ImGui::Dummy(ImVec2(0, theme::kS1));
  st.lay.footMin = ImGui::GetCursorScreenPos();
  bool nameEnter = false;
  if (!folderMode) {
    // Save: the box holds the name only; the chosen type's extension is shown faintly at its
    // end and added when the file is written.
    const bool hideExt = fd.save && !fd.exts.empty();
    const std::string ext = hideExt ? savedExt(fd.name, fd.exts) : std::string();
    const std::string shown = fd.name.substr(0, fd.name.size() - ext.size());
    const std::string typeExt = hideExt ? (ext.empty() ? fd.exts[0] : ext) : std::string();
    static char nameBuf[512];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", shown.c_str());
    if (st.focusName && !appearing) {
      ImGui::SetKeyboardFocusHere();
      st.focusName = false;
      st.selectName = 3;
    }
    // The whole name is selected, so typing replaces it (ImGui would keep the old cursor when the
    // text is the same as the last time the box had the focus).
    const ImGuiInputTextFlags nameFlags = ImGuiInputTextFlags_EnterReturnsTrue | (st.selectName > 0 ? ImGuiInputTextFlags_AutoSelectAll : 0);
    if (st.selectName > 0) --st.selectName;
    ImGui::SetNextItemWidth(-1);
    nameEnter = ImGui::InputTextWithHint("##name", i18n::tr("File name"), nameBuf, sizeof nameBuf, nameFlags);
    st.boxText = nameBuf;
    st.boxExt = typeExt;
    if (hideExt && nameBuf[0]) {
      const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
      const ImVec2 tw = ImGui::CalcTextSize(typeExt.c_str());
      const float tx = std::max(mn.x + ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(nameBuf).x + 2.0f,
                                mx.x - ImGui::GetStyle().FramePadding.x - tw.x);
      if (tx + tw.x <= mx.x - 2.0f)
        ImGui::GetWindowDrawList()->AddText(ImVec2(tx, mn.y + (mx.y - mn.y - tw.y) * 0.5f), P.textFaint, typeExt.c_str());
    }
    if (std::strcmp(nameBuf, shown.c_str()) != 0) {
      std::string typed = nameBuf;
      if (hideExt && !typed.empty()) {
        if (fd.nameForType) typed = fd.nameForType(typed, fd.typeIndex);
        else if (savedExt(typed, fd.exts).empty()) typed += typeExt;
      }
      fd.name = typed;
      if (ImGui::IsItemActive()) st.selected.clear();
    }
    ui::FocusRing();
  } else {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitTextMiddle(fd.dir.c_str(), ImGui::GetContentRegionAvail().x));
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  // Save dialogs with types: the name always carries the chosen type's extension.
  const std::string target = fd.name.empty() ? std::string() : (fs::u8path(fd.dir) / fs::u8path(savedName(fd))).u8string();
  if (!fd.confirmOverwrite.empty() && fd.confirmOverwrite != target) fd.confirmOverwrite.clear();
  const char* okLabel = !fd.confirmOverwrite.empty() ? "Replace" : fd.save ? "Save" : folderMode ? "Choose this folder" : "Open";
  const float okW = std::max(96.0f, ImGui::CalcTextSize(i18n::tr(okLabel)).x + 24);
  // J-52: file types (Open: all projects / one format; Save as: the format written).
  if (!fd.types.empty()) {
    fd.typeIndex = std::clamp(fd.typeIndex, 0, static_cast<int>(fd.types.size()) - 1);
    const float typeW = std::max(120.0f, std::min(360.0f, ImGui::GetContentRegionAvail().x - (96 + okW + theme::kS2 * 3)));
    if (ui::BeginCombo("##types", i18n::tr(fd.types[static_cast<size_t>(fd.typeIndex)].label.c_str()), typeW)) {  // J-78: translated labels
      for (size_t i = 0; i < fd.types.size(); ++i)
        if (ImGui::Selectable(i18n::tr(fd.types[i].label.c_str()), static_cast<int>(i) == fd.typeIndex)) setType(fd, static_cast<int>(i));
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, theme::kS2);
  } else if (!folderMode && !fd.exts.empty() && !fd.save) {
    // The kinds of file shown, as a quiet note.
    std::string kinds;
    for (const auto& e : fd.exts) kinds += (kinds.empty() ? "" : " ") + e;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(kinds.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x - (96 + okW + theme::kS2 * 3))));
    ImGui::SameLine(0, theme::kS2);
  }
  if (!fd.confirmOverwrite.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.hot), "%s", i18n::tr("Already exists"));
    ImGui::SameLine(0, theme::kS2);
  }
  {
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - (96 + theme::kS2 + okW);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
  }
  const bool cancel = ui::TextButton("Cancel", false, 96) || (::gl::keys::escapePressed() && !st.editingPath);
  ImGui::SameLine(0, theme::kS2);
  bool ok = ui::PrimaryButton(okLabel, okW) || okDev;
  st.lay.footMax = ImGui::GetItemRectMax();
  if (!fd.confirmOverwrite.empty()) ui::Hint((fd.name + " already exists. Replace it?").c_str());
  // Enter: in the name field, or on a selected file (a selected folder opened above).
  if (nameEnter && !fd.name.empty()) ok = true;
  if (!enterOnRow && !io.WantTextInput && !appearing && !st.editingPath && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
      (!fd.name.empty() || folderMode))
    ok = true;
  if (ok && chosen.empty()) {
    if (folderMode) {
      const int s = selectedIndex(st);
      chosen = s >= 0 && st.rows[static_cast<size_t>(s)].isDir ? pathOf(a, st.rows[static_cast<size_t>(s)].name) : fd.dir;
    } else if (!fd.name.empty()) {
      chosen = target;
    }
  }
  // Saving over an existing file asks once (the button turns into Replace).
  std::error_code ec;
  if (fd.save && !chosen.empty() && chosen != fd.confirmOverwrite &&
      (remote(chosen) ? remotefs::probeWithin(chosen, 2000).exists : fs::exists(fs::u8path(chosen), ec))) {
    fd.confirmOverwrite = chosen;
    fd.name = fs::u8path(chosen).filename().u8string();
    chosen.clear();
  }
  if (!chosen.empty()) {
    auto cb = fd.onOk;
    remember(a, st, folderMode ? chosen : fs::u8path(chosen).parent_path().u8string());
    ImGui::CloseCurrentPopup();
    a.stopPreview();
    st.previewing.clear();
    if (cb) cb(chosen);
  } else if (cancel || !keep) {
    a.stopPreview();
    st.previewing.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  State& st = S();
  if (args.empty()) return;
  const std::string& cmd = args[0];
  if (cmd == "open") {
    a.doc.markSaved();
    a.openDialog();
    if (args.size() > 1) a.fileDialog.dir = args[1];
    return;
  }
  if (cmd == "saveas") {
    a.saveAs();
    if (args.size() > 1) a.fileDialog.dir = args[1];
    return;
  }
  if (cmd == "state") {
    const FileDialogState& fd = a.fileDialog;
    const auto r = [](ImVec2 mn, ImVec2 mx) {
      char b[96];
      std::snprintf(b, sizeof b, "%.0f %.0f %.0f %.0f", mn.x, mn.y, mx.x, mx.y);
      return std::string(b);
    };
    static const char* const keys[] = {"name", "size", "kind", "modified"};
    std::printf("filedialog frame %d shown %d\n", frame, st.shown ? 1 : 0);
    std::printf("  dir %s\n  sort %s %s hidden %d filter \"%s\"\n  name \"%s\" selected \"%s\"\n", fd.dir.c_str(), keys[std::clamp(st.sortKey, 0, 3)],
                st.sortDesc ? "desc" : "asc", st.hidden ? 1 : 0, st.filter, fd.name.c_str(), st.selected.c_str());
    for (const auto& e : st.rows) std::printf("  row %s %s %lld\n", e.isDir ? "dir" : "file", e.name.c_str(), static_cast<long long>(e.size));
    static const char* const sections[] = {"drive", "place", "favourite", "recent"};
    for (const auto& s : st.side) std::printf("  side %s \"%s\" %s\n", sections[std::clamp(s.section, 0, 3)], s.name.c_str(), s.path.c_str());
    std::printf("  back %zu forward %zu\n", st.back.size(), st.fwd.size());
    std::printf("  namebox \"%s\" ext \"%s\" type %d confirm \"%s\"\n", st.boxText.c_str(), st.boxExt.c_str(), fd.typeIndex, fd.confirmOverwrite.c_str());
    std::printf("  layout dialog %s\n  layout bar %s\n  layout side %s\n  layout list %s\n  layout foot %s\n", r(st.lay.dlgMin, st.lay.dlgMax).c_str(),
                r(st.lay.barMin, st.lay.barMax).c_str(), r(st.lay.sideMin, st.lay.sideMax).c_str(), r(st.lay.listMin, st.lay.listMax).c_str(),
                r(st.lay.footMin, st.lay.footMax).c_str());
    std::printf("filedialog end\n");
    std::fflush(stdout);
    return;
  }
  st.pending.push_back(args);
}

}  // namespace gl::filedlgui

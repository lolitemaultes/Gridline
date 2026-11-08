// Gridline — browser: current project, recent files, plugin database, preset
// libraries (plugin / channel / mixer), FL Studio's shipped content read in
// place (packs, impulses, scores, demo projects), Gridline's own content
// folders and user folders. Smart search over everything (type: tag: bpm:
// key: len:), sample preview with tempo sync, waveform + file info panel,
// tags, drag & drop to every window (BrowserUi.h).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "CollabUi.h"
#include "../core/I18n.h"
#include "../core/PatternAutomation.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Presets.h"
#include "../core/FileOps.h"
#include "../core/ProjectInterop.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginGroups.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "../core/RemoteFs.h"
#include "StemsUi.h"
#include "ExternalDragUi.h"
#include "HostedUi.h"
#include "Theme.h"
#include "UrlImportUi.h"
#include "Widgets.h"
#include "Windows.h"
#include "../core/Platform.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;
namespace role = theme::role;
using browse::Kind;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// ------------------------------------------------------------ directories

struct DirCache {
  std::vector<fs::path> dirs, files;
  double stamp = -1;
  std::shared_ptr<const remotefs::Listing> from;  // K-42: on a web drive: the listing this was made from
  bool pending = false;                           // and none has arrived yet
};
std::map<std::string, DirCache> dirCache;

const DirCache& listDir(const fs::path& dir, double now) {
  DirCache& d = dirCache[dir.string()];
  auto byNameOf = [](const fs::path& a, const fs::path& b) { return lower(a.filename().string()) < lower(b.filename().string()); };
  if (remotefs::isRemote(dir.string())) {  // K-42: a folder on a web drive is read off the interface thread
    const remotefs::DirState st = remotefs::listAsync(dir.string(), 30.0);
    d.pending = st.pending && !st.listing;
    if (st.listing && st.listing != d.from) {
      d = DirCache{};
      d.from = st.listing;
      d.stamp = now;
      for (const auto& e : st.listing->entries) {
        if (e.isDir) d.dirs.push_back(dir / fs::u8path(e.name));
        else if (browse::listable(browse::kindOf(e.name))) d.files.push_back(dir / fs::u8path(e.name));
      }
      std::sort(d.dirs.begin(), d.dirs.end(), byNameOf);
      std::sort(d.files.begin(), d.files.end(), byNameOf);
    }
    return d;
  }
  if (d.stamp >= 0 && now - d.stamp < 3.0) return d;
  d = DirCache{};
  d.stamp = now;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::error_code e2;
    if (it->is_directory(e2))
      d.dirs.push_back(it->path());
    else if (browse::listable(browse::kindOf(it->path().string())))
      d.files.push_back(it->path());
  }
  auto byName = [](const fs::path& a, const fs::path& b) { return lower(a.filename().string()) < lower(b.filename().string()); };
  std::sort(d.dirs.begin(), d.dirs.end(), byName);
  std::sort(d.files.begin(), d.files.end(), byName);
  return d;
}

// fs::is_directory for paths the browser checks every frame (one stat call
// each): cached for a few seconds like the listings (J-25).
struct DirFlag {
  bool dir = false;
  double stamp = -1;
};
std::map<std::string, DirFlag> dirFlags;

bool isDirCached(const fs::path& dir, double now) {
  if (remotefs::isRemote(dir.string())) {  // K-42: asked off the interface thread; a folder until it says otherwise
    const remotefs::Probe p = remotefs::probe(dir.string(), 30.0);
    return !p.known || p.isDir;
  }
  DirFlag& e = dirFlags[dir.string()];
  if (e.stamp < 0 || now - e.stamp >= 3.0) {
    std::error_code ec;
    e.dir = fs::is_directory(dir, ec);
    e.stamp = now;
  }
  return e.dir;
}

std::vector<std::string> folders;
bool foldersLoaded = false;

void loadFolders(App& a) {
  if (foldersLoaded) return;
  foldersLoaded = true;
  std::ifstream in(fs::path(a.configDir()) / "browser-folders.txt");
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) folders.push_back(l);
  if (folders.empty()) {
    const char* h = gl::platform::homeEnv();
    std::string home = h ? h : ".";
    for (const char* sub : {"Music", "Documents", "Downloads"})
      if (fs::is_directory(fs::path(home) / sub)) folders.push_back((fs::path(home) / sub).string());
  }
}
void saveFolders(App& a) {
  std::error_code ec;
  fs::create_directories(a.configDir(), ec);
  std::ofstream out(fs::path(a.configDir()) / "browser-folders.txt");
  for (const auto& f : folders) out << f << "\n";
}

// ------------------------------------------------------------- state

enum Tab { TabAll, TabProject, TabPlugins, TabSounds, TabStarred, TabCount };
int tab = TabAll;
std::string selPath;  // file shown in the info panel

constexpr float kRowH = 24.0f;

// Dev (screenshots): GRIDLINE_BROWSER_EXPAND="Packs,Drums" opens nodes with
// those labels; GRIDLINE_BROWSER_SELECT=<path> shows a file's info panel.
bool devOpen(const std::string& label) {
  static const std::string list = [] {
    const char* e = std::getenv("GRIDLINE_BROWSER_EXPAND");
    return e ? "," + std::string(e) + "," : std::string();
  }();
  return !list.empty() && list.find("," + label + ",") != std::string::npos;
}
constexpr const char* kStarTag = "starred";
std::string devSection;  // dev: the section the rows drawn belong to (K-18 "browser rows")

ImU32 roleCol(uint32_t r, float a = 0.9f) { return theme::litRole(r, a); }

// One browser row: icon, name (ellipsised), optional right-aligned detail.
// Returns true when clicked; `dbl` reports a double-click.
// K-16: `starHit` (plugin rows): hovering shows the star, a click on it is reported there
// (the row's own click is not).
bool browserRow(const char* id, ui::Icon icon, const char* name, const char* detail, float indent, bool* dbl, ImU32 iconCol = 0,
                bool selected = false, bool star = false, bool* starHit = nullptr) {
  const auto& P = pal();
  App& a = app();
  ImGui::PushID(id);
  ImVec2 q = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  bool clicked = ImGui::InvisibleButton("##row", ImVec2(std::max(w, 1.0f), kRowH));
  bool hov = ImGui::IsItemHovered();
  if (dbl) *dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (selected) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + kRowH), theme::withAlpha(P.hot, 0.16f), theme::kRadiusSm);
  if (hov || ImGui::IsItemActive()) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + kRowH), ImGui::IsItemActive() ? P.surface700 : P.surface800, theme::kRadiusSm);
  if (selected) dl->AddRectFilled(q, ImVec2(q.x + 2, q.y + kRowH), P.hot);
  float x = q.x + indent;
  // Tree guide: dotted stem down the indent with a tick into the row.
  if (indent > 0) {
    const ImU32 gc = theme::withAlpha(P.lineStrong, 0.7f);
    const float gx = std::round(x - 3);
    for (float y = q.y; y < q.y + kRowH; y += 3) dl->AddLine(ImVec2(gx, y), ImVec2(gx, y + 1), gc);
    for (float xx = gx + 2; xx < x + 2; xx += 3) dl->AddLine(ImVec2(xx, q.y + kRowH * 0.5f), ImVec2(xx + 1, q.y + kRowH * 0.5f), gc);
  }
  // Icons are tinted by kind (instruments, effects, note effects, folders, audio).
  ImU32 ic = iconCol ? iconCol : P.textFaint;
  if (!iconCol) switch (icon) {
      case ui::Icon::Plug: ic = theme::litRole(role::kDraw, 0.85f); break;
      case ui::Icon::Mixer: ic = theme::litRole(role::kSlice, 0.85f); break;
      case ui::Icon::Piano: ic = theme::litRole(role::kSnap, 0.85f); break;
      case ui::Icon::Folder: ic = theme::litRole(role::kView, 0.85f); break;
      case ui::Icon::Wave: ic = theme::litRole(role::kPaint, 0.85f); break;
      default: break;
    }
  ui::drawIcon(dl, icon, ImVec2(x + 8, q.y + kRowH * 0.5f), 12, hov ? P.text : ic);
  float detailW = 0;
  if (starHit) {
    const bool sh = hov && ImGui::GetIO().MousePos.x >= q.x + w - 20;
    *starHit = clicked && sh;
    if (*starHit) clicked = false;
    if (star || hov) {
      if (sh) dl->AddCircleFilled(ImVec2(q.x + w - 10, q.y + kRowH * 0.5f), 8.0f, theme::withAlpha(P.surface950, 0.55f), 16);
      browserui::drawStar(dl, ImVec2(q.x + w - 10, q.y + kRowH * 0.5f), 4.5f, star ? P.hot : sh ? P.text : P.textFaint, star);
    }
    detailW += 16;
  } else if (star) {
    browserui::drawStar(dl, ImVec2(q.x + w - 10, q.y + kRowH * 0.5f), 4.5f, P.hot, true);
    detailW += 16;
  }
  if (detail && *detail && w - indent > 150) {
    float dw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, detail).x;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q.x + w - dw - 8 - detailW, std::round(q.y + (kRowH - theme::kFontLabel) * 0.5f)),
                P.textFaint, detail);
    detailW += dw + 12;
  }
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(x + 22, std::round(q.y + (kRowH - 13) * 0.5f) - 1),
                      w - indent - 30 - detailW, selected ? P.text : P.text, name);
  ImGui::PopID();
  return clicked;
}

// Collapsible section header (top level): chevron, coloured folder, title, count.
// `title` is English (and the open state's id); it is shown translated unless
// it is a name from disk (`translate` false).
// K-18: the last section header drawn (its rect), for root folder menus and drops.
struct HeaderHit {
  ImGuiID id = 0;
  ImVec2 min, max;
};
HeaderHit lastHeader;

bool sectionHeader(const char* title, int count, bool defaultOpen, uint32_t roleColour = role::kView, ui::Icon icon = ui::Icon::Folder,
                   bool translate = true) {
  const auto& P = pal();
  App& a = app();
  ImGuiStorage* st = ImGui::GetStateStorage();
  ImGuiID id = ImGui::GetID(title);
  bool open = st->GetBool(id, defaultOpen || devOpen(title));
  ImVec2 q = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::PushID(title);
  if (ImGui::InvisibleButton("##hdr", ImVec2(std::max(w, 1.0f), 26))) {
    open = !open;
    st->SetBool(id, open);
  }
  bool hov = ImGui::IsItemHovered();
  lastHeader = HeaderHit{ImGui::GetItemID(), q, ImVec2(q.x + w, q.y + 26)};
  devSection = title;
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 26), hov ? P.surface700 : theme::withAlpha(P.surface800, 0.6f), theme::kRadiusSm);
  ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(q.x + 9, q.y + 13), 10, open ? P.hot : P.textDim);
  ui::drawIcon(dl, icon, ImVec2(q.x + 25, q.y + 13), 12, roleCol(roleColour));
  const std::string label = translate ? i18n::tr(title) : title;  // J-61: tree roots are translated
  const std::string shown = label.substr(0, label.find("##"));     // "##" suffix: id only
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption + 1, ImVec2(q.x + 37, q.y + 6), w - 37 - 30, P.text, shown.c_str());
  if (count >= 0) {
    std::string c = std::to_string(count);
    float cw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, c.c_str()).x;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q.x + w - cw - 8, q.y + 7), P.textFaint, c.c_str());
  }
  ImGui::Dummy(ImVec2(0, 1));
  return open;
}

// Expandable folder row inside a section (state kept per key).
std::set<std::string> forceOpen;  // K-18: folders to open (a new folder made in them)

bool treeNode(const std::string& key, const char* label, int count, float indent, uint32_t roleColour = role::kView) {
  ImGuiStorage* st = ImGui::GetStateStorage();
  ImGuiID id = ImGui::GetID(key.c_str());
  if (forceOpen.erase(key)) st->SetBool(id, true);
  bool open = st->GetBool(id, devOpen(label));
  std::string c = count >= 0 ? std::to_string(count) : "";
  if (browserRow(key.c_str(), ui::Icon::Folder, label, c.c_str(), indent, nullptr, roleCol(roleColour, open ? 1.0f : 0.8f))) {
    open = !open;
    st->SetBool(id, open);
  }
  return open;
}

void emptyLine(const char* text, float indent = 20) {
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", i18n::tr(text));
  ImGui::Dummy(ImVec2(0, 4));
}

std::string fmtSeconds(double s) {
  char b[32];
  if (s >= 60) std::snprintf(b, sizeof b, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
  else std::snprintf(b, sizeof b, s < 10 ? "%.2fs" : "%.1fs", s);
  return b;
}

ui::Icon iconOf(Kind k) {
  switch (k) {
    case Kind::Audio: return ui::Icon::Wave;
    case Kind::Midi:
    case Kind::Score: return ui::Icon::Piano;
    case Kind::Project:
    case Kind::Flp: return ui::Icon::Song;
    case Kind::Chain: return ui::Icon::Mixer;
    case Kind::Preset:
    case Kind::Fst: return ui::Icon::Settings;
    case Kind::Image: return ui::Icon::Paint;  // J-184: pictures (Pixel Tone)
    default: return ui::Icon::File;
  }
}

ImU32 iconColOf(Kind k) {
  switch (k) {
    case Kind::Audio: return roleCol(role::kPaint, 0.85f);
    case Kind::Midi:
    case Kind::Score: return roleCol(role::kSnap, 0.85f);
    case Kind::Project:
    case Kind::Flp: return roleCol(role::kPlay, 0.85f);
    case Kind::Chain: return roleCol(role::kSlice, 0.85f);
    case Kind::Preset:
    case Kind::Fst: return roleCol(role::kStamp, 0.85f);
    case Kind::Image: return roleCol(role::kDraw, 0.85f);
    default: return pal().textFaint;
  }
}

// Right-aligned detail of a file row: length / tempo for audio, kind otherwise.
std::string fileDetail(const fs::path& p, Kind k) {
  if (k == Kind::Audio) {
    browse::FileMeta m;
    if (browserui::index().lookup(p.string(), m)) {
      if (m.bpm > 0) return std::to_string(static_cast<int>(std::lround(m.bpm))) + "bpm";
      if (m.seconds > 0) return fmtSeconds(m.seconds);
    }
    return "";
  }
  switch (k) {
    case Kind::Flp: return "FLP";
    case Kind::Fst: return "FST";
    case Kind::Midi: return "MIDI";
    default: return "";
  }
}

bool isStarred(const std::string& path) {
  auto t = browserui::index().tagsOf(path);
  return std::find(t.begin(), t.end(), kStarTag) != t.end();
}

void toggleStar(const std::string& path) {
  auto t = browserui::index().tagsOf(path);
  auto it = std::find(t.begin(), t.end(), kStarTag);
  if (it == t.end()) t.push_back(kStarTag);
  else t.erase(it);
  browserui::index().setTags(path, t);
}

// ------------------------------------------------------ K-18: file operations
// Rename (inline field in the tree), New folder, move by dragging onto a
// folder, Move to the trash (asked first). Only inside the user's folders and
// Gridline's own folders; the open project's references follow (one undo step
// that also moves the files back), as do the index, tags and recent files.

// Dev (--dev-input "browser rows"): the file and folder rows drawn last frame, by section.
struct DevRow {
  std::string section, path;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
bool devRowsOn = false;
std::vector<DevRow> devRowsNow, devRowsLast;
void devRow(const std::string& path) {
  if (!devRowsOn) return;
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  devRowsNow.push_back(DevRow{devSection, path, a.x, a.y, b.x, b.y});
}

struct FileOpsState {
  std::string renaming;     // the row showing the name field
  bool focus = false;       // focus the field next frame
  char buf[256] = "";
  std::string error;        // shown under the field
  std::string trashAsk;     // asked about in the trash dialog
  bool trashOpen = false;
  std::string browserDrag;  // the browser file row being dragged (moves inside the browser)
} fops;

// The file operations run from inside the tree (Enter in a name field, a drop on a folder, a
// row's menu), while dirTree is walking the very listing they make stale: the listings are
// only forgotten at the start of the next browser frame, when nothing walks them.
bool listingsStale = false;
void clearListings() { listingsStale = true; }
void forgetStaleListings() {
  if (!listingsStale) return;
  listingsStale = false;
  dirCache.clear();
  dirFlags.clear();
}

// The project's referenced files (channels, note effects, mixer slots), listed again
// only when the song changes.
const std::vector<std::string>& projectFiles(App& a) {
  static std::vector<std::string> files;
  static uint64_t rev = ~0ull;
  static const Song* song = nullptr;
  if (rev != a.doc.revision() || song != &a.doc.song()) {
    rev = a.doc.revision();
    song = &a.doc.song();
    files = referencedFiles(a.doc.song());
  }
  return files;
}

int refsTo(App& a, const std::string& path) {
  int n = 0;
  for (const auto& f : projectFiles(a)) n += fileops::isInside(f, path) ? 1 : 0;
  return n;
}

// Folders whose contents may be renamed, moved and deleted: the user's browser
// folders and Gridline's own folders.
std::vector<std::string> editableRoots(App& a) {
  std::vector<std::string> r(folders.begin(), folders.end());
  for (const char* n : {"Clipboard files", "Sliced audio"}) r.push_back(browserui::contentDir(a, n));
  r.push_back(recordedDir(a));
  r.push_back(renderedDir(a));
  r.push_back(urlimportui::importedDir(a));
  r.push_back(stemsDir(a));
  r.push_back(editedDir(a));
  r.push_back(seltmannLoopsDir(a));
  if (!a.projectPath.empty()) r.push_back((fs::path(a.projectPath).parent_path() / "Audio").string());
  return r;
}

// Why `path` cannot be renamed, moved or deleted ("" when it can). `asParent`: the
// folder receives a new folder or a moved item instead (a browser root may).
std::string lockedWhy(App& a, const std::string& path, bool asParent = false) {
  const std::string fl = browserui::flDataDir();
  if (!fl.empty() && fileops::isInside(path, fl)) return i18n::N_("FL Studio's shipped content is read-only");
  bool inRoot = false, isRoot = false;
  for (const auto& r : editableRoots(a)) {
    if (!fileops::isInside(path, r)) continue;
    inRoot = true;
    isRoot = isRoot || fileops::isInside(r, path);
  }
  if (!inRoot) return i18n::N_("Read-only here: files can be changed in your own folders and Gridline's folders");
  if (isRoot && !asParent) return i18n::N_("This folder is one of the browser's own folders");
  if (!asParent && !a.projectPath.empty() && fileops::isInside(a.projectPath, path))
    return i18n::N_("The open project is saved here: use File > Save as to move it");
  if (collabui::active(a)) {
    const std::string shared = a.projectPath.empty() ? std::string() : fs::path(a.projectPath).parent_path().string();
    if ((!shared.empty() && fileops::isInside(path, shared)) || (!asParent && refsTo(a, path) > 0))
      return i18n::N_("Shared with the collaborative project: the others refer to it by this name");
  }
  const std::string dir = asParent ? path : fs::path(path).parent_path().string();
  if (!platform::writable(dir)) return i18n::N_("The folder is read-only");
  return "";
}

// A menu item that is disabled (with the reason as a tooltip) when `why` is set.
bool opItem(const char* label, const std::string& why, const char* shortcut = nullptr) {
  const bool ok = why.empty();
  const bool hit = ImGui::MenuItem(i18n::tr(label), shortcut, false, ok);
  if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", i18n::tr(why.c_str()));
  return hit && ok;
}

void startRename(const std::string& path) {
  std::error_code ec;
  const fs::path p(path);
  const bool folder = fs::is_directory(p, ec);
  const std::string shown = folder || p.extension().empty() ? p.filename().string() : p.stem().string();
  std::snprintf(fops.buf, sizeof fops.buf, "%s", shown.c_str());
  fops.renaming = path;
  fops.focus = true;
  fops.error.clear();
}

// After a rename or move on disk: the index, the user folders, the selection, the
// recent files and (one undo step) the open project's references follow.
void afterMoves(App& a, const fileops::Result& r, const std::string& label) {
  if (r.moves.empty()) return;
  const fileops::Move top = r.moves.front();
  const int refs = refsTo(a, top.first);
  for (const auto& m : r.moves) browserui::index().movePath(m.first, m.second);
  bool foldersChanged = false;
  for (auto& f : folders) foldersChanged = fileops::retargetPath(f, top.first, top.second) || foldersChanged;
  if (foldersChanged) saveFolders(a);
  fileops::retargetPath(selPath, top.first, top.second);
  std::vector<fileops::Move> recents;
  for (const auto& rf : a.recentFiles) {
    std::string p = rf;
    if (fileops::retargetPath(p, top.first, top.second)) recents.emplace_back(rf, p);
  }
  for (const auto& [from, to] : recents) a.moveRecent(from, to);
  clearListings();
  if (refs > 0)
    a.doc.execute(std::make_unique<fileops::FileMoveCommand>(label, r.moves, std::vector<fileops::Move>{top},
                                                              [](const std::string& from, const std::string& to) {
                                                                browserui::index().movePath(from, to);
                                                                fileops::retargetPath(selPath, from, to);
                                                                clearListings();
                                                              }));
  std::string msg = label + ": " + fs::path(top.second).filename().string();
  if (refs > 0) msg += "  \xC2\xB7  " + std::to_string(refs) + " " + i18n::tr(refs == 1 ? "project reference updated" : "project references updated");
  a.notify(msg);
}

// Renames `path` to the name typed (a file keeps its extension when none is typed).
// Returns the error to show ("" when done).
std::string commitRename(App& a, const std::string& path, std::string name) {
  while (!name.empty() && name.back() == ' ') name.pop_back();
  std::error_code ec;
  const fs::path p(path);
  const bool folder = fs::is_directory(p, ec);
  const std::string ext = folder ? std::string() : p.extension().string();
  if (!ext.empty() && (name.size() < ext.size() || lower(name.substr(name.size() - ext.size())) != lower(ext))) name += ext;
  if (name == p.filename().string()) return "";
  if (std::string why = lockedWhy(a, path); !why.empty()) return i18n::tr(why.c_str());
  const fileops::Result r = fileops::rename(path, name);
  if (!r.ok) return i18n::tr(r.error.c_str());
  afterMoves(a, r, i18n::tr("Renamed"));
  return "";
}

void moveIntoFolder(App& a, const std::string& src, const std::string& dir) {
  std::string why = lockedWhy(a, src);
  if (why.empty()) why = lockedWhy(a, dir, true);
  if (!why.empty()) {
    a.notify(i18n::tr(why.c_str()), true);
    return;
  }
  const fileops::Result r = fileops::moveInto(src, dir);
  if (!r.ok) {
    a.notify(i18n::tr(r.error.c_str()), true);
    return;
  }
  forceOpen.insert(dir);
  afterMoves(a, r, i18n::tr("Moved"));
}

void newFolderIn(App& a, const std::string& dir) {
  if (std::string why = lockedWhy(a, dir, true); !why.empty()) {
    a.notify(i18n::tr(why.c_str()), true);
    return;
  }
  const fileops::Result r = fileops::makeFolder(dir, i18n::tr("New folder"));
  if (!r.ok) {
    a.notify(i18n::tr(r.error.c_str()), true);
    return;
  }
  clearListings();
  forceOpen.insert(dir);
  startRename(r.path);
}

// The name field of the row being renamed, drawn instead of its name. True when drawn.
bool renameRow(App& a, const fs::path& p, float indent, ui::Icon icon, ImU32 iconCol) {
  if (fops.renaming.empty() || fops.renaming != p.string()) return false;
  const auto& P = pal();
  ImGui::PushID("##renaming");
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + w, q.y + kRowH), P.surface800, theme::kRadiusSm);
  dl->AddRectFilled(q, ImVec2(q.x + 2, q.y + kRowH), P.hot);
  const float x = q.x + indent;
  ui::drawIcon(dl, icon, ImVec2(x + 8, q.y + kRowH * 0.5f), 12, iconCol);
  std::error_code ec;
  const bool folder = fs::is_directory(p, ec);
  const std::string ext = folder ? std::string() : p.extension().string();
  const float extW = ext.empty() ? 0.0f : std::round(a.fontRegular->CalcTextSizeA(theme::kFontCaption + 1, 1e9f, 0, ext.c_str()).x) + theme::kS1;
  const float fieldH = kRowH - 4;
  ImGui::SetCursorScreenPos(ImVec2(x + 18, q.y + 2));
  ImGui::SetNextItemWidth(std::max(40.0f, w - indent - 22 - extW - theme::kS1));
  if (fops.focus) {
    ImGui::SetKeyboardFocusHere();
    fops.focus = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(theme::kS1, std::max(0.0f, std::floor((fieldH - ImGui::GetFontSize()) * 0.5f))));
  const bool enter = ImGui::InputText("##name", fops.buf, sizeof fops.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  const bool escape = ::gl::keys::escapePressed(false);
  const bool left = ImGui::IsItemDeactivated();
  ImGui::PopStyleVar();
  ui::FocusRing();
  ui::Hint(folder ? "Type the new name \xC2\xB7 Enter: rename \xC2\xB7 Esc: cancel"
                  : "Type the new name (the extension stays) \xC2\xB7 Enter: rename \xC2\xB7 Esc: cancel");
  const ImVec2 f1 = ImGui::GetItemRectMax();
  if (!ext.empty())
    dl->AddText(a.fontRegular, theme::kFontCaption + 1, ImVec2(f1.x + theme::kS1, std::round(q.y + (kRowH - 13) * 0.5f) - 1), P.textFaint, ext.c_str());
  float h = kRowH;
  if (!fops.error.empty()) {
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x + 18, q.y + kRowH + 2), w - indent - 22, theme::lit(P.danger), fops.error.c_str());
    h += 18;
  }
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + h));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
  const std::string path = p.string();
  if (escape) {
    fops.renaming.clear();
    fops.error.clear();
  } else if (enter || left) {
    const std::string err = commitRename(a, path, fops.buf);
    if (err.empty()) {
      fops.renaming.clear();
    } else if (enter) {
      fops.error = err;
      fops.focus = true;
    } else {
      fops.renaming.clear();  // clicked away: the name stays as it was, the reason as a notification
      a.notify(err, true);
    }
  }
  return true;
}

// The item a drag inside the browser carries: a browser file row or folder ("" otherwise).
std::string browserDragItem(const ImGuiPayload* pl) {
  if (!pl || !pl->Data) return "";
  const std::string v(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
  if (pl->IsDataType("GL_BFOLDER")) return v;
  if ((pl->IsDataType("GL_FILE") || pl->IsDataType("GL_ITEM")) && v == fops.browserDrag) return v;
  return "";
}

// Between Begin/EndDragDropTarget of a folder: files and folders dragged inside the
// browser move into it (the reason shows when they cannot).
void acceptMoveInto(App& a, const std::string& dir) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  const std::string src = browserDragItem(pl);
  if (src.empty() || fileops::isInside(dir, src) || fileops::isInside(fs::path(src).parent_path().string(), dir)) return;
  std::string why = lockedWhy(a, src);
  if (why.empty()) why = lockedWhy(a, dir, true);
  if (!why.empty()) {
    a.setHint(i18n::tr(why.c_str()));
    return;
  }
  a.setHint(std::string(i18n::tr("Drop to move it into ")) + fs::path(dir).filename().string());
  if (ImGui::AcceptDragDropPayload(pl->DataType)) moveIntoFolder(a, src, dir);
}

// A folder row (in a tree): drag source, drop target, its menu.
void folderRowExtras(App& a, const fs::path& dir) {
  const std::string d = dir.string();
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("GL_BFOLDER", d.data(), d.size());
    ui::DragPreview(ui::Icon::Folder, dir.filename().string().c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    acceptMoveInto(a, d);
    ImGui::EndDragDropTarget();
  }
  ImGui::PushID(d.c_str());
  if (ImGui::BeginPopupContextItem("##dctx")) {
    ImGui::TextDisabled("%s", theme::fitText(dir.filename().string().c_str(), 260));
    const std::string why = lockedWhy(a, d);
    if (opItem("New folder", lockedWhy(a, d, true))) newFolderIn(a, d);
    if (opItem("Rename", why)) startRename(d);
    if (opItem("Move to the trash...", why)) fops.trashAsk = d, fops.trashOpen = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

// A browser root's section header (just drawn): drops move into it, right-click: New folder.
void rootHeaderExtras(App& a, const std::string& dir, const HeaderHit& h) {
  if (h.id != 0 && ImGui::GetDragDropPayload() && ImGui::BeginDragDropTargetCustom(ImRect(h.min, h.max), h.id)) {
    acceptMoveInto(a, dir);
    ImGui::EndDragDropTarget();
  }
}
bool rootHeaderMenu(const HeaderHit& h, const char* id) {
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(h.min, h.max) &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    ImGui::OpenPopup(id);
  return ImGui::BeginPopup(id);
}

// The trash dialog's action: `fops.trashAsk` to the system trash.
void trashNow(App& a) {
  const std::string path = fops.trashAsk;
  fops.trashAsk.clear();
  if (path.empty()) return;
  const std::string why = lockedWhy(a, path);
  if (!why.empty()) {
    a.notify(i18n::tr(why.c_str()), true);
    return;
  }
  const char* testTrash = std::getenv("GRIDLINE_TRASH_DIR");  // tests: a trash folder of their own
  const fileops::Result r = fileops::moveToTrash(path, testTrash ? testTrash : "");
  if (!r.ok) {
    a.notify(i18n::tr(r.error.c_str()), true);
    return;
  }
  for (const auto& m : r.moves) browserui::index().movePath(m.first, "");
  if (fileops::isInside(selPath, path)) selPath.clear();
  clearListings();
  a.notify(std::string(i18n::tr("Moved to the trash: ")) + fs::path(path).filename().string());
}

// "Move to the trash?" (asked first; the trash keeps the item, the project's
// references to it go missing).
void trashDialog(App& a) {
  if (fops.trashOpen) {
    ImGui::OpenPopup("##trashask");
    fops.trashOpen = false;
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.35f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
  const bool open = ImGui::BeginPopupModal("##trashask", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) return;
  const auto& P = pal();
  constexpr float kW = 340.0f, kButtonW = 112.0f;
  const fs::path p(fops.trashAsk);
  std::error_code ec;
  const bool folder = fs::is_directory(p, ec);
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Move to the trash?"));
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::TextUnformatted(theme::fitText(p.filename().string().c_str(), kW));
  if (folder) {
    static std::string counted;
    static size_t files = 0;
    if (counted != fops.trashAsk) {
      counted = fops.trashAsk;
      files = 0;
      for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && files < 100000;
           it.increment(ec))
        files += it->is_regular_file(ec) ? 1 : 0;
    }
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("A folder with %zu files"), files);
  }
  const int refs = refsTo(a, fops.trashAsk);
  if (refs > 0) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
    ImGui::TextColored(theme::vec(theme::lit(P.danger)), i18n::tr("The open project uses it (%d references): they will be missing until it is put back."), refs);
    ImGui::PopTextWrapPos();
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("It can be restored from the system trash."));
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kW - ui::ButtonRowWidth(2, kButtonW));
  const bool cancel = ui::TextButton("Cancel", false, kButtonW) || ::gl::keys::escapePressed();
  ImGui::SameLine(0, theme::kS2);
  if (ui::DangerButton("Move to trash", true, kButtonW)) {
    trashNow(a);
    ImGui::CloseCurrentPopup();
  } else if (cancel) {
    fops.trashAsk.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void fileContextMenu(App& a, const std::string& ps) {
  if (!ImGui::BeginPopupContextItem("##fctx")) return;
  ImGui::TextDisabled("%s", theme::fitText(fs::path(ps).filename().string().c_str(), 260));
  if (ImGui::MenuItem(isStarred(ps) ? i18n::tr("Remove star") : i18n::tr("Star"))) toggleStar(ps);
  if (ImGui::MenuItem(i18n::tr("Open / add"))) browserui::openFile(a, ps);
  if (browse::kindOf(ps) == Kind::Audio) {
    if (ImGui::MenuItem(i18n::tr("Preview"))) browserui::previewFile(a, ps);
    if (ImGui::MenuItem(i18n::tr("Open in Seltmann"))) {  // J-107: a new Seltmann channel analyses and chops it
      a.showNewInstrument(a.addInstrument("gl.seltmann", ps));
    }
    if (ImGui::MenuItem(i18n::tr("Add to playlist as audio clip"))) {
      browserui::DropPlace pl;
      pl.tick = a.engine.position() > 0 ? static_cast<Tick>(a.engine.position()) : 0;
      browserui::performDrop(a, ps, browse::DropTarget::Playlist, pl);
    }
    if (ImGui::MenuItem(i18n::tr("Load as impulse on the selected insert"))) browserui::performDrop(a, ps, browse::DropTarget::MixerTrack, {});
    if (ImGui::MenuItem(i18n::tr("Split stems..."))) stemsui::openForFile(a, ps);  // J-191
  }
  // K-18: rename, new folder beside it, the trash (only in writable user / Gridline folders).
  ImGui::Separator();
  const std::string why = lockedWhy(a, ps);
  const std::string parent = fs::path(ps).parent_path().string();
  if (opItem("Rename", why)) startRename(ps);
  if (opItem("New folder", lockedWhy(a, parent, true))) newFolderIn(a, parent);
  if (opItem("Move to the trash...", why)) fops.trashAsk = ps, fops.trashOpen = true;
  ImGui::EndPopup();
}

void fileRow(App& a, const fs::path& p, float indent) {
  const std::string ps = p.string();
  const Kind k = browse::kindOf(ps);
  // Presets show their names; files keep their extension.
  const bool named = k == Kind::Preset || k == Kind::Chain || k == Kind::Shape || k == Kind::Fst;
  std::string name = named ? p.stem().string() : p.filename().string();
  if (renameRow(a, p, indent, iconOf(k), iconColOf(k))) return;  // K-18
  bool dbl = false;
  const std::string det = fileDetail(p, k);
  const bool sel = ps == selPath;
  bool clicked = browserRow(ps.c_str(), iconOf(k), name.c_str(), det.c_str(), indent, &dbl, iconColOf(k), sel, isStarred(ps));
  if (clicked) {
    selPath = ps;
    if (k == Kind::Audio) browserui::previewFile(a, ps);
  }
  if (dbl) browserui::openFile(a, ps);
  switch (k) {
    case Kind::Audio:
      ui::Hint("Click: preview \xC2\xB7 double-click: new sampler channel \xC2\xB7 drag onto the rack, playlist (audio clip), a mixer slot (impulse) or a sampler");
      break;
    case Kind::Midi: ui::Hint("Double-click or drag onto the rack: import \xC2\xB7 drag onto the piano roll: add the notes to the pattern"); break;
    case Kind::Flp: ui::Hint("Double-click to open this FL Studio project"); break;
    case Kind::Project: ui::Hint("Double-click to open this project"); break;
    case Kind::Fst:
    case Kind::Preset:
      ui::Hint("Double-click: load into the open plugin window (or a new channel / the selected insert) \xC2\xB7 drag onto a plugin window, the rack or a mixer slot");
      break;
    case Kind::Chain: ui::Hint("Double-click: load on the selected mixer track \xC2\xB7 drag onto a mixer strip"); break;
    case Kind::Image: ui::Hint("Drag onto a Pixel Tone editor or channel to play this picture \xC2\xB7 double-click: load into the open Pixel Tone"); break;
    default: break;
  }
  devRow(ps);
  ImGui::PushID(ps.c_str());
  fileContextMenu(a, ps);
  ImGui::PopID();
  browserui::fileDragSource(ps);
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) fops.browserDrag = ps;  // K-18: a move inside the browser
}

void dirTree(App& a, const fs::path& dir, int depth, float baseIndent = 12.0f) {
  if (depth > 12) return;
  const DirCache& d = listDir(dir, a.now);
  const float indent = baseIndent + depth * 14.0f;
  if (d.pending) emptyLine("Reading this folder", indent + 20.0f);
  for (const auto& sub : d.dirs) {
    std::string key = sub.string();
    if (renameRow(a, sub, indent, ui::Icon::Folder, roleCol(role::kView))) continue;  // K-18
    const bool open = treeNode(key, sub.filename().string().c_str(), -1, indent);
    devRow(key);
    folderRowExtras(a, sub);  // K-18: drag, drop into, menu
    if (open) dirTree(a, sub, depth + 1, baseIndent);
  }
  for (const auto& f : d.files) fileRow(a, f, indent);
}

// Folder of FL Studio's shipped content (read in place) as a tree node.
void flFolderNode(App& a, const char* label, const fs::path& dir, float indent, uint32_t roleColour) {
  if (!isDirCached(dir, a.now)) return;
  if (treeNode(dir.string(), label, -1, indent, roleColour)) dirTree(a, dir, 0, indent + 14);
}

// --------------------------------------------------------------- presets

struct PresetCache {
  std::map<std::string, std::vector<PresetEntry>> byType;
  std::vector<std::string> types;
  std::vector<PresetEntry> chains;
  double stamp = -100;
};
PresetCache& presets(App& a) {
  static PresetCache c;
  if (a.now - c.stamp > 5.0) {
    c = PresetCache{};
    c.stamp = a.now;
    const std::string root = presetRoot(a.configDir());
    c.types = browse::libraryTypes(root);
    for (const auto& t : c.types) c.byType[t] = listPresets(root, t);
    c.chains = listChains(root);
  }
  return c;
}

void presetTreeRows(App& a, const std::vector<PresetEntry>& entries, const browse::PresetNode& node, const std::string& key, float indent) {
  for (const auto& f : node.folders) {
    const std::string k = key + "/" + f.name;
    if (treeNode(k, f.name.c_str(), static_cast<int>(f.count()), indent)) presetTreeRows(a, entries, f, k, indent + 14);
  }
  for (size_t i : node.presets) fileRow(a, entries[i].file, indent);
}

// Library presets of the plugin types that pass `want`, one node per plugin.
int presetLibrary(App& a, const std::function<bool(const PluginInfo&)>& want, float indent, const char* keyBase) {
  PresetCache& pc = presets(a);
  int shown = 0;
  for (const auto& type : pc.types) {
    const PluginInfo* info = findPlugin(type);
    if (!info || !want(*info)) continue;
    const auto& entries = pc.byType[type];
    if (entries.empty()) continue;
    ++shown;
    const std::string key = std::string(keyBase) + type;
    if (treeNode(key, info->name.c_str(), static_cast<int>(entries.size()), indent, info->kind == PluginKind::Generator ? role::kDraw : role::kSlice)) {
      static std::map<std::string, std::pair<size_t, browse::PresetNode>> trees;
      auto& t = trees[type];
      if (t.first != entries.size() || t.second.count() != entries.size()) t = {entries.size(), browse::buildPresetTree(entries)};
      presetTreeRows(a, entries, t.second, key, indent + 14);
    }
  }
  return shown;
}

// ---------------------------------------------------------------- plugins

void pluginRow(App& a, const PluginInfo& p, float indent, const char* detail, bool fav) {
  bool dbl = false;
  ui::Icon icon = p.kind == PluginKind::Generator ? ui::Icon::Plug : p.kind == PluginKind::Effect ? ui::Icon::Mixer : ui::Icon::Piano;
  bool starHit = false;
  browserRow(p.type.c_str(), icon, p.name.c_str(), detail, indent, &dbl, 0, false, fav, &starHit);
  if (starHit) host::setFavourite(p.type, !fav);  // K-16: the star toggles the favourite
  if (starHit) dbl = false;
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("GL_PLUGIN", p.type.data(), p.type.size());
    ui::DragPreview(icon, p.name.c_str());
    ImGui::EndDragDropSource();
  }
  ImGui::PushID(p.type.c_str());
  if (ImGui::BeginPopupContextItem("##pctx")) {
    if (ImGui::MenuItem(fav ? i18n::tr("Remove from favourites") : i18n::tr("Add to favourites"))) host::setFavourite(p.type, !fav);
    if (ImGui::MenuItem(i18n::tr("Plugin picker"), keys::label("view.pluginPicker"))) browserui::togglePluginPicker();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  ObjectId target = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  if (dbl) {
    if (p.kind == PluginKind::Generator) {
      a.showNewInstrument(a.addInstrument(p.type));
    } else if (p.kind == PluginKind::Effect) {
      if (const MixerTrack* t = a.doc.song().mixerTrack(a.selMixer))
        for (int k = 0; k < kEffectSlots; ++k)
          if (t->slots[static_cast<size_t>(k)].plugin.empty()) {
            a.doc.setSlotPlugin(t->id, k, p.type);
            a.fxEditor = {t->id, k};
            break;
          }
    } else if (target != kNoId) {
      a.doc.addNoteFx(target, p.type);
      a.settingsChannel = target;
    }
  }
  ui::Hint(p.name + " \xC2\xB7 " + p.category + "   " +
           (p.kind == PluginKind::Generator ? "(double-click or drag onto the channel rack to add \xC2\xB7 star: favourite)"
            : p.kind == PluginKind::Effect  ? "(double-click to insert on the selected mixer track, or drag onto a strip or slot)"
                                            : "(double-click to add to the selected channel's note effects)"));
}

// Built-in plugins of one kind, by category (collapsible).
void builtinPlugins(App& a, PluginKind kind, float indent, const char* keyBase) {
  const host::PluginPrefs prefs = host::pluginPrefs();
  std::map<std::string, std::vector<const PluginInfo*>> byCat;
  for (const auto& p : pluginCatalog())
    if (p.kind == kind && !p.hidden) byCat[p.category.empty() ? "Other" : p.category].push_back(&p);
  for (auto& [cat, list] : byCat) {
    std::sort(list.begin(), list.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
    const std::string key = std::string(keyBase) + cat;
    if (!treeNode(key, cat.c_str(), static_cast<int>(list.size()), indent, browserui::categoryRole(cat, static_cast<int>(kind)))) continue;
    for (const PluginInfo* p : list) pluginRow(a, *p, indent + 14, nullptr, prefs.favourites.count(p->type) > 0);
  }
}

// K-73: the installed plugins the browser lists, by group and sorted. Worked out again only when the
// plugins, their grouping or the plugin settings change: with a few thousand plugins, doing it every
// frame (and drawing every row of an open group) held each frame up.
struct InstalledList {
  size_t count = ~size_t(0);
  const void* groups = nullptr;
  uint64_t prefsGen = ~uint64_t(0);
  std::shared_ptr<const host::GroupSnapshot> keep;  // the grouping the key points at, alive
  int total = 0;
  struct Group {
    std::string key, title;
    std::vector<std::pair<const PluginInfo*, const char*>> plugins;  // plugin, format label
  };
  std::vector<Group> byGroup;
};

void buildInstalled(App& a, InstalledList& out, const std::vector<const PluginInfo*>& all) {
  std::vector<const PluginInfo*> list;
  for (const PluginInfo* p : all) {
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    if (!host::listedInBrowser(p->type)) continue;  // J-147: one format per plugin, no Gridline Suite builds
    if (!collabui::pluginAllowed(a, p->type)) continue;  // J-111: not in a collaborative project
    list.push_back(p);
  }
  const host::PluginPrefs prefs = host::pluginPrefs();
  auto group = [&prefs](const PluginInfo* p) {
    auto it = prefs.categories.find(p->type);
    return it == prefs.categories.end() ? p->category : it->second;
  };
  std::map<std::string, std::vector<const PluginInfo*>> byGroup;
  for (const PluginInfo* p : list)
    byGroup[(p->kind == PluginKind::Generator ? "Instruments \xC2\xB7 " : "Effects \xC2\xB7 ") + group(p)].push_back(p);
  out.total = static_cast<int>(list.size());
  out.byGroup.clear();
  for (auto& [g, ps] : byGroup) {
    std::sort(ps.begin(), ps.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
    InstalledList::Group grp{"##inst" + g, g, {}};
    for (const PluginInfo* p : ps) {
      std::string fmtId, fmtUid;
      const host::Format* fmt = host::splitType(p->type, fmtId, fmtUid) ? host::findFormat(fmtId) : nullptr;
      grp.plugins.push_back({p, fmt ? fmt->label : ""});
    }
    out.byGroup.push_back(std::move(grp));
  }
}

void installedPlugins(App& a, float indent) {
  const auto& P = pal();
  const host::ScanStatus ss = host::scanStatus();
  static InstalledList cached;
  {
    const std::vector<const PluginInfo*> all = dynamicPlugins();
    std::shared_ptr<const host::GroupSnapshot> groups = host::pluginGroups();
    const uint64_t pg = host::pluginPrefsGeneration();
    // A collaborative project decides per plugin (its session changes): worked out every frame.
    if (collabui::active(a) || cached.count != all.size() || cached.groups != groups.get() || cached.prefsGen != pg) {
      buildInstalled(a, cached, all);
      cached.count = all.size();
      cached.groups = groups.get();
      cached.keep = std::move(groups);
      cached.prefsGen = pg;
    }
  }
  if (collabui::active(a)) {
    ImGui::Indent(indent);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Plugins that are not part of Gridline are hidden in a collaborative project."));
    ImGui::PopTextWrapPos();
    ImGui::Unindent(indent);
  }
  const host::PluginPrefs prefs = host::pluginPrefs();
  if (!treeNode("##installed", "Installed", cached.total, indent, role::kMenu)) return;
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + 14);
  if (ss.running && !ss.searching.empty()) {  // K-71: a big folder takes a while to search
    ui::Label("Looking for plugins");
  } else if (ss.running) {
    const std::string cur = host::scanCurrentLabel(ss);
    ui::Label(("Scanning " + std::to_string(std::min(ss.done + 1, std::max(1, ss.total))) + "/" + std::to_string(ss.total) + "  " + cur).c_str());
  } else {
    if (ui::TextButton("Rescan", false, 0, "Look for new or updated plugins (right-click: rescan everything, including failed ones)"))
      { host::startScan(host::selfExecutablePath(), false); a.scanByUser = true; }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { host::startScan(host::selfExecutablePath(), true); a.scanByUser = true; }
    ImGui::SameLine(0, 6);
    if (ui::TextButton("Manage", false, 0, "Plugin manager: folders, formats, failed plugins, sandbox, favourites")) a.showPluginManager = true;
    if (ss.failed > 0) {
      ImGui::SameLine(0, 8);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
      ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%d failed"), ss.failed);
      if (ImGui::IsItemHovered()) {
        std::string tip = "Blacklisted (crashed, hung or failed to load; rescanned when updated):";
        for (const auto& [b, why] : host::blacklist()) tip += "\n" + fs::path(b).filename().string() + ": " + why;
        ui::Tooltip(tip.c_str());
      }
    }
  }
  if (cached.byGroup.empty()) {
    emptyLine("No plugins found", indent + 20);
    return;
  }
  for (const InstalledList::Group& g : cached.byGroup) {
    if (!treeNode(g.key, g.title.c_str(), static_cast<int>(g.plugins.size()), indent + 14, role::kMenu)) continue;
    for (const auto& [p, label] : g.plugins) {
      // K-73: a row out of sight takes its place without being drawn (an open group of hundreds).
      if (!ImGui::IsRectVisible(ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), kRowH))) {
        ImGui::Dummy(ImVec2(1, kRowH));
        continue;
      }
      pluginRow(a, *p, indent + 28, label, prefs.favourites.count(p->type) > 0);
    }
  }
}

// ---------------------------------------------------------- the sections

void sectionCurrentProject(App& a) {
  const Song& s = a.doc.song();
  if (!sectionHeader("Current project", -1, true, role::kPlay, ui::Icon::Song)) return;
  const float in = 12;
  if (treeNode("##cp.pat", "Patterns", static_cast<int>(s.patterns.size()), in, role::kPlay)) {
    for (const auto& p : s.patterns) {
      bool dbl = false;
      const std::string id = "##pat" + std::to_string(p.id);
      if (browserRow(id.c_str(), ui::Icon::Pattern, p.name.c_str(), nullptr, in + 14, &dbl, theme::rgb(p.color), p.id == a.curPattern))
        a.selectPattern(p.id);
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("GL_PATTERN", &p.id, sizeof p.id);
        ui::DragPreview(ui::Icon::Pattern, p.name.c_str(), theme::rgb(p.color));
        ImGui::EndDragDropSource();
      }
      ui::Hint("Click: select the pattern \xC2\xB7 drag onto the playlist to place it");
    }
  }
  if (treeNode("##cp.ch", "Channels", static_cast<int>(s.channels.size()), in, role::kDraw)) {
    for (const auto& c : s.channels) {
      bool dbl = false;
      const PluginInfo* info = findPlugin(c.plugin.type);
      const std::string id = "##ch" + std::to_string(c.id);
      if (browserRow(id.c_str(), ui::Icon::Plug, c.name.c_str(), info ? info->name.c_str() : "", in + 14, &dbl, theme::rgb(c.color),
                     c.id == a.selChannel))
        a.selectChannel(c.id);
      if (dbl) a.settingsChannel = c.id;
      ui::Hint("Click: select \xC2\xB7 double-click: open the channel");
    }
  }
  int fx = 0;
  for (const auto& m : s.mixer)
    for (const auto& sl : m.slots) fx += !sl.plugin.empty();
  if (treeNode("##cp.fx", "Effects", fx, in, role::kSlice)) {
    for (size_t i = 0; i < s.mixer.size(); ++i)
      for (int k = 0; k < kEffectSlots; ++k) {
        const auto& sl = s.mixer[i].slots[static_cast<size_t>(k)];
        const PluginInfo* info = sl.plugin.empty() ? nullptr : findPlugin(sl.plugin.type);
        if (!info) continue;
        bool dbl = false;
        const std::string id = "##fx" + std::to_string(s.mixer[i].id) + "." + std::to_string(k);
        const std::string where = mixerTrackName(s, i);
        if (browserRow(id.c_str(), ui::Icon::Mixer, info->name.c_str(), where.c_str(), in + 14, &dbl, 0,
                       a.fxEditor == std::make_pair(s.mixer[i].id, k)))
          a.selMixer = s.mixer[i].id;
        if (dbl) a.fxEditor = {s.mixer[i].id, k};
        ui::Hint("Click: select the mixer track \xC2\xB7 double-click: open the effect");
      }
  }
  size_t inPatterns = 0;  // J-173: pattern automation clips are listed too ("in <pattern>")
  for (const auto& p : s.patterns) inPatterns += p.automation.size();
  if (treeNode("##cp.auto", "Automation", static_cast<int>(s.automations.size() + inPatterns), in, role::kGraph)) {
    for (const auto& ac : s.automations) {
      bool dbl = false;
      const std::string id = "##ac" + std::to_string(ac.id);
      browserRow(id.c_str(), ui::Icon::Draw, ac.name.c_str(), nullptr, in + 14, &dbl, theme::rgb(ac.color));
      if (dbl) openAutomationEditor(a, ac.id);
      ui::Hint("Double-click: edit the automation clip");
    }
    ObjectId openPat = kNoId;
    ParamId openTgt{};
    for (const auto& p : s.patterns)
      for (const auto& ac : p.automation) {
        bool dbl = false;
        const std::string id = "##pa" + std::to_string(p.id) + "." + std::to_string(ac.owner) + "." + std::to_string(ac.index);
        const std::string name = patternAutoName(s, ac), where = std::string(i18n::tr("in")) + " " + p.name;
        browserRow(id.c_str(), ui::Icon::Draw, name.c_str(), where.c_str(), in + 14, &dbl,
                   theme::rgb(ac.color == AutomationClip{}.color ? p.color : ac.color));
        if (dbl) openPat = p.id, openTgt = ParamId{ac.owner, ac.index};
        ui::Hint("Automation in a pattern \xC2\xB7 double-click: edit it");
      }
    if (openPat != kNoId) openPatternAutomation(a, openPat, openTgt);
  }
  // Samples used by the project's channels (preview, drag elsewhere), listed
  // again only when the project changes.
  // K-17: every audio file the project refers to (channel samples, audio clips' channels,
  // the audio editor's edited takes, impulse responses, recordings, renders, stems, slices).
  std::vector<std::string> used;
  for (const auto& f : projectFiles(a))
    if (browse::kindOf(f) == Kind::Audio) used.push_back(f);
  if (treeNode("##cp.smp", "Samples used", static_cast<int>(used.size()), in, role::kPaint))
    for (const auto& u : used) fileRow(a, u, in + 14);
  // Audio saved with the project (recordings, audio editor renders).
  if (!a.projectPath.empty()) {
    const fs::path audio = fs::path(a.projectPath).parent_path() / "Audio";
    if (isDirCached(audio, a.now) && treeNode("##cp.audio", "Project audio", -1, in, role::kDelete)) dirTree(a, audio, 0, in + 14);
  }
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionRecent(App& a) {
  if (!sectionHeader("Recent files", static_cast<int>(a.recentFiles.size()), false, role::kView)) return;
  for (const auto& r : a.recentFiles) fileRow(a, fs::path(r), 12);
  if (a.recentFiles.empty()) emptyLine("No recent projects");
}

void sectionPluginDatabase(App& a) {
  if (!sectionHeader("Plugin database", -1, true, role::kDraw, ui::Icon::Plug)) return;
  {  // K-16: the starred plugins (built-in and installed) first, draggable like the others
    const host::PluginPrefs prefs = host::pluginPrefs();
    std::vector<const PluginInfo*> favs;
    for (const auto& t : prefs.favourites)
      if (const PluginInfo* p = findPlugin(t); p && collabui::pluginAllowed(a, p->type)) favs.push_back(p);
    std::sort(favs.begin(), favs.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
    if (treeNode("##pdb.fav", "Favourites", static_cast<int>(favs.size()), 12, role::kStamp)) {
      for (const PluginInfo* p : favs) pluginRow(a, *p, 26, nullptr, true);
      if (favs.empty()) emptyLine("Click a plugin's star (or right-click: Add to favourites)", 40);
    }
  }
  installedPlugins(a, 12);
  if (treeNode("##pdb.gen", "Generators", -1, 12, role::kDraw)) builtinPlugins(a, PluginKind::Generator, 26, "##pdb.g.");
  if (treeNode("##pdb.fx", "Effects", -1, 12, role::kSlice)) builtinPlugins(a, PluginKind::Effect, 26, "##pdb.e.");
  if (treeNode("##pdb.nfx", "Note effects", -1, 12, role::kSnap)) builtinPlugins(a, PluginKind::NoteFx, 26, "##pdb.n.");
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionPluginPresets(App& a) {
  if (!sectionHeader("Plugin presets", -1, false, role::kStamp)) return;
  int n = presetLibrary(a, [](const PluginInfo& p) { return p.kind != PluginKind::Generator; }, 12, "##pp.");
  const std::string fl = browserui::flDataDir();
  if (!fl.empty()) flFolderNode(a, "FL Studio originals (.fst)", fs::path(fl) / "Patches" / "Plugin presets", 12, role::kMenu);
  if (n == 0 && fl.empty()) emptyLine("Save presets from a plugin window");
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionChannelPresets(App& a) {
  if (!sectionHeader("Channel presets", -1, false, role::kSnap)) return;
  int n = presetLibrary(a, [](const PluginInfo& p) { return p.kind == PluginKind::Generator; }, 12, "##chp.");
  const std::string fl = browserui::flDataDir();
  if (!fl.empty()) flFolderNode(a, "FL Studio channel presets", fs::path(fl) / "Patches" / "Channel presets", 12, role::kMenu);
  if (n == 0 && fl.empty()) emptyLine("Save instrument presets from a channel");
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionMixerPresets(App& a) {
  if (!sectionHeader("Mixer presets", -1, false, role::kSlice, ui::Icon::Mixer)) return;
  PresetCache& pc = presets(a);
  if (!pc.chains.empty() && treeNode("##mp.chains", "Effect chains", static_cast<int>(pc.chains.size()), 12, role::kSlice)) {
    static std::pair<size_t, browse::PresetNode> tree;
    if (tree.first != pc.chains.size() || tree.second.count() != pc.chains.size()) tree = {pc.chains.size(), browse::buildPresetTree(pc.chains)};
    presetTreeRows(a, pc.chains, tree.second, "##mp.chains", 26);
  }
  const std::string fl = browserui::flDataDir();
  if (!fl.empty()) flFolderNode(a, "FL Studio mixer presets", fs::path(fl) / "Patches" / "Mixer presets", 12, role::kMenu);
  if (pc.chains.empty() && fl.empty()) emptyLine("Save chains from a mixer track's menu");
  ImGui::Dummy(ImVec2(0, 4));
}

// A folder section (FL content or Gridline's own folders).
void folderSection(App& a, const char* title, const fs::path& dir, uint32_t roleColour, const char* emptyText, bool create = false) {
  if (!isDirCached(dir, a.now)) {
    if (!create) return;
  }
  {
    ImGuiStorage* st = ImGui::GetStateStorage();
    if (forceOpen.erase(dir.string())) st->SetBool(ImGui::GetID(title), true);
  }
  const bool open = sectionHeader(title, -1, false, roleColour);
  const HeaderHit hh = lastHeader;
  if (ImGui::IsMouseHoveringRect(hh.min, hh.max) && ImGui::IsWindowHovered()) a.setHint(dir.string() + "   (right-click: new folder)");
  rootHeaderExtras(a, dir.string(), hh);  // K-18: drops move into it
  ImGui::PushID(title);
  if (rootHeaderMenu(hh, "##rootctx")) {
    ImGui::TextDisabled("%s", theme::fitText(dir.string().c_str(), 300));
    if (opItem("New folder", lockedWhy(a, dir.string(), true))) newFolderIn(a, dir.string());
    ImGui::EndPopup();
  }
  ImGui::PopID();
  if (!open) return;
  std::error_code ec;
  if (create && !isDirCached(dir, a.now)) fs::create_directories(dir, ec);
  const DirCache& d = listDir(dir, a.now);
  if (d.dirs.empty() && d.files.empty()) emptyLine(d.pending ? "Reading this folder" : emptyText);
  else dirTree(a, dir, 0);
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionProjects(App& a) {
  const std::string fl = browserui::flDataDir();
  if (fl.empty()) return;
  if (!sectionHeader("Projects", -1, false, role::kLoop, ui::Icon::Song)) return;
  flFolderNode(a, "Demo projects", fs::path(fl) / "Demo projects", 12, role::kLoop);
  flFolderNode(a, "Templates", fs::path(fl) / "Templates", 12, role::kLoop);
  ImGui::Dummy(ImVec2(0, 4));
}

void sectionUserFolders(App& a) {
  for (size_t i = 0; i < folders.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    fs::path f(folders[i]);
    if (forceOpen.erase(folders[i])) ImGui::GetStateStorage()->SetBool(ImGui::GetID(f.filename().string().c_str()), true);
    bool open = sectionHeader(f.filename().string().c_str(), -1, false, role::kView, ui::Icon::Folder, false);
    const HeaderHit hh = lastHeader;
    if (ImGui::IsMouseHoveringRect(hh.min, hh.max) && ImGui::IsWindowHovered())
      a.setHint(folders[i] + "   " + i18n::tr("(right-click: new folder, refresh, remove from the browser)"));
    rootHeaderExtras(a, folders[i], hh);  // K-18: drops move into it
    if (rootHeaderMenu(hh, "##fmenu")) {
      ImGui::TextDisabled("%s", folders[i].c_str());
      if (opItem("New folder", lockedWhy(a, folders[i], true))) newFolderIn(a, folders[i]);
      if (ImGui::MenuItem(i18n::tr("Refresh"))) {
        dirCache.clear();
        browserui::refreshIndex(a);
      }
      if (ImGui::MenuItem(i18n::tr("Remove from browser"))) {
        folders.erase(folders.begin() + static_cast<long>(i));
        saveFolders(a);
        ImGui::EndPopup();
        ImGui::PopID();
        break;
      }
      ImGui::EndPopup();
    }
    if (open) dirTree(a, f, 0);
    ImGui::PopID();
  }
}

void sectionStarred(App& a) {
  const host::PluginPrefs prefs = host::pluginPrefs();
  std::vector<const PluginInfo*> favs;
  for (const auto& t : prefs.favourites)
    if (const PluginInfo* p = findPlugin(t)) favs.push_back(p);
  std::sort(favs.begin(), favs.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
  if (sectionHeader("Favourite plugins", static_cast<int>(favs.size()), true, role::kDraw, ui::Icon::Plug)) {
    for (const PluginInfo* p : favs) pluginRow(a, *p, 12, nullptr, true);
    if (favs.empty()) emptyLine("Click a plugin's star (or right-click: Add to favourites)");
  }
  browse::Query q;
  q.tags = {kStarTag};
  auto files = browserui::index().search(q, 400);
  if (sectionHeader("Starred files", static_cast<int>(files.size()), true, role::kMenu)) {
    for (const auto& m : files) fileRow(a, m.path, 12);
    if (files.empty()) emptyLine("Right-click a file: Star");
  }
}

// ---------------------------------------------------------------- search

struct Results {
  std::string text;
  uint64_t gen = ~0ull;
  double stamp = -1;
  std::vector<browse::FileMeta> files;
};

int searchResults(App& a, const std::string& text) {
  static Results r;
  const uint64_t gen = browserui::index().generation();
  if (r.text != text || (r.gen != gen && a.now - r.stamp > 0.6)) {
    r.text = text;
    r.gen = gen;
    r.stamp = a.now;
    r.files = browserui::index().search(browse::parseQuery(text), 300);
  }
  const browse::Query q = browse::parseQuery(text);
  const host::PluginPrefs prefs = host::pluginPrefs();
  int shown = 0;
  // Plugins (built-in, then hosted).
  std::vector<const PluginInfo*> plugs;
  for (const auto& p : pluginCatalog())
    if (!p.hidden && browse::matchesPlugin(p, q, prefs.favourites.count(p.type) > 0)) plugs.push_back(&p);
  for (const PluginInfo* p : dynamicPlugins()) {
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    if (!host::listedInBrowser(p->type)) continue;  // J-147
    if (!collabui::pluginAllowed(a, p->type)) continue;  // J-111
    if (browse::matchesPlugin(*p, q, prefs.favourites.count(p->type) > 0)) plugs.push_back(p);
  }
  if (!plugs.empty() && sectionHeader("Plugins##res", static_cast<int>(plugs.size()), true, role::kDraw, ui::Icon::Plug)) {
    for (const PluginInfo* p : plugs) pluginRow(a, *p, 12, p->category.c_str(), prefs.favourites.count(p->type) > 0);
    ImGui::Dummy(ImVec2(0, 4));
  }
  shown += static_cast<int>(plugs.size());
  // Files grouped by kind.
  static const std::pair<Kind, const char*> groups[] = {
      {Kind::Audio, i18n::N_("Samples")}, {Kind::Preset, i18n::N_("Presets")}, {Kind::Fst, i18n::N_("FL Studio presets")},
      {Kind::Chain, i18n::N_("Effect chains")}, {Kind::Midi, i18n::N_("MIDI files")}, {Kind::Score, i18n::N_("Scores")},
      {Kind::Project, i18n::N_("Projects")}, {Kind::Flp, i18n::N_("FL Studio projects")},
      {Kind::Shape, i18n::N_("Automation shapes")}, {Kind::Image, i18n::N_("Pictures")},
  };
  for (const auto& [kind, title] : groups) {
    int n = 0;
    for (const auto& m : r.files) n += m.kind == kind;
    if (!n) continue;
    shown += n;
    if (!sectionHeader((std::string(title) + "##res").c_str(), n, true, kind == Kind::Audio ? role::kPaint : role::kStamp)) continue;
    for (const auto& m : r.files)
      if (m.kind == kind) fileRow(a, m.path, 12);
    ImGui::Dummy(ImVec2(0, 4));
  }
  return shown;
}

// ------------------------------------------------------------ info panel

constexpr float kInfoH = 128.0f;

void infoPanel(App& a) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + kInfoH), P.surface900);
  dl->AddLine(p0, ImVec2(p0.x + w, p0.y), P.line);
  browse::FileMeta m;
  const bool known = browserui::index().lookup(selPath, m);
  if (!known) m = browse::Index::describe(selPath, false);
  const float pad = theme::kS2;
  // Title + star + close.
  const std::string name = fs::path(selPath).filename().string();
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption + 1, ImVec2(p0.x + pad, p0.y + 6), w - 2 * pad - 48, P.text, name.c_str());
  ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 46, p0.y + 3));
  {
    const bool st = std::find(m.tags.begin(), m.tags.end(), kStarTag) != m.tags.end();
    ImVec2 sp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##star", ImVec2(20, 20))) toggleStar(selPath);
    browserui::drawStar(dl, ImVec2(sp.x + 10, sp.y + 10), 6, st ? P.hot : P.textFaint, st);
    ui::Hint(st ? "Starred (click to remove the star)" : "Star this file (Starred tab)");
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 24, p0.y + 3));
  if (ui::IconButton("closeinfo", ui::Icon::Close, false, 20, "Close the file info")) selPath.clear();
  // Waveform mini-view.
  const ImVec2 w0(p0.x + pad, p0.y + 26), w1(p0.x + w - pad, p0.y + 26 + 40);
  dl->AddRectFilled(w0, w1, P.well, theme::kRadiusSm);
  const std::vector<float> peaks = browse::decodePeaks(m.peaks);
  if (!peaks.empty()) {
    const float mid = (w0.y + w1.y) * 0.5f, hh = (w1.y - w0.y) * 0.5f - 2;
    const float bw = (w1.x - w0.x) / static_cast<float>(peaks.size());
    const ImU32 wc = theme::rgb(role::kPaint, 0.9f);
    for (size_t i = 0; i < peaks.size(); ++i) {
      float x = w0.x + bw * static_cast<float>(i);
      float h = std::max(0.5f, peaks[i] * hh);
      dl->AddRectFilled(ImVec2(x + 0.5f, mid - h), ImVec2(x + std::max(1.0f, bw - 0.5f), mid + h), wc);
    }
    dl->AddLine(ImVec2(w0.x, mid), ImVec2(w1.x, mid), theme::withAlpha(wc, 0.5f));
  } else {
    const char* t = m.kind == Kind::Audio ? "Click to preview and analyse" : browse::kindName(m.kind);
    float tw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, t).x;
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2((w0.x + w1.x - tw) * 0.5f, (w0.y + w1.y - theme::kFontCaption) * 0.5f), P.textFaint, t);
  }
  ImGui::SetCursorScreenPos(w0);
  if (ImGui::InvisibleButton("##wave", ImVec2(w1.x - w0.x, w1.y - w0.y)) && m.kind == Kind::Audio) browserui::previewFile(a, selPath);
  browserui::fileDragSource(selPath);
  ui::Hint("Click to preview \xC2\xB7 drag like the file row");
  // Facts line.
  std::string facts;
  auto add = [&facts](const std::string& s) {
    if (s.empty()) return;
    if (!facts.empty()) facts += "  \xC2\xB7  ";
    facts += s;
  };
  if (m.seconds > 0) add(fmtSeconds(m.seconds));
  if (m.bpm > 0) {
    char b[24];
    std::snprintf(b, sizeof b, m.bpm == std::round(m.bpm) ? "%.0f BPM" : "%.1f BPM", m.bpm);
    add(b);
  }
  if (m.key >= 0) add(browse::keyName(m.key, m.minor));
  if (m.rate > 0) {
    char b[24];
    std::snprintf(b, sizeof b, "%.1f kHz", m.rate / 1000.0);
    add(b);
  }
  if (m.channels > 0) add(m.channels == 1 ? "mono" : "stereo");
  if (m.kind != Kind::Audio) add(browse::kindName(m.kind));
  if (m.size > 0) {
    char b[24];
    std::snprintf(b, sizeof b, m.size >= (1u << 20) ? "%.1f MB" : "%.0f KB", m.size >= (1u << 20) ? m.size / 1048576.0 : m.size / 1024.0);
    add(b);
  }
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p0.x + pad, w1.y + 5), P.textDim, theme::fitText(facts.c_str(), w - 2 * pad));
  // Tags: user tags as removable chips, then automatic ones (faint), then the add field.
  float x = p0.x + pad, y = w1.y + 24;
  int removeAt = -1;
  for (size_t i = 0; i < m.tags.size(); ++i) {
    if (m.tags[i] == kStarTag) continue;
    const std::string t = "#" + m.tags[i];
    float tw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, t.c_str()).x + 12;
    if (x + tw > p0.x + w - pad - 70) break;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##tag", ImVec2(tw, 18))) removeAt = static_cast<int>(i);
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tw, y + 18), hov ? theme::withAlpha(P.danger, 0.35f) : theme::withAlpha(P.hot, 0.22f), 9);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 6, y + 2), P.text, t.c_str());
    if (hov) ui::Hint("Click to remove this tag");
    x += tw + 4;
  }
  for (const auto& t : browse::autoTags(selPath)) {
    const std::string s = "#" + t;
    float tw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, s.c_str()).x + 12;
    if (x + tw > p0.x + w - pad - 70) break;
    dl->AddRect(ImVec2(x, y), ImVec2(x + tw, y + 18), P.line, 9);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 6, y + 2), P.textFaint, s.c_str());
    x += tw + 4;
  }
  if (removeAt >= 0) {
    auto t = m.tags;
    t.erase(t.begin() + removeAt);
    browserui::index().setTags(selPath, t);
  }
  static char tagBuf[48] = "";
  ImGui::SetCursorScreenPos(ImVec2(p0.x + w - pad - 66, y - 2));
  ImGui::SetNextItemWidth(66);
  if (ImGui::InputTextWithHint("##addtag", i18n::tr("+ tag"), tagBuf, sizeof tagBuf, ImGuiInputTextFlags_EnterReturnsTrue) && tagBuf[0]) {
    auto t = m.tags;
    t.push_back(tagBuf);
    browserui::index().setTags(selPath, t);
    tagBuf[0] = 0;
  }
  ui::Hint("Add a tag (Enter) \xC2\xB7 search with tag:<name>");
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + kInfoH));
}

// Tab strip under the search field (All / Project / Plugins / Sounds / Starred).
void tabStrip(App& a) {
  const auto& P = pal();
  static const char* labels[TabCount] = {"All", "Project", "Plugins", "Sounds", "Starred"};
  static const char* hints[TabCount] = {"Everything", "Current project, recent files, projects", "Plugin database and presets",
                                        "Packs, impulses, scores, recordings and your folders", "Favourite plugins and starred files"};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 24;
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface900);
  dl->AddLine(ImVec2(p0.x, p0.y + h - 1), ImVec2(p0.x + w, p0.y + h - 1), P.line);
  const float tw = w / TabCount;
  for (int i = 0; i < TabCount; ++i) {
    ImVec2 q(p0.x + tw * static_cast<float>(i), p0.y);
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##tab", ImVec2(tw, h))) tab = i;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) ui::Hint(hints[i]);
    const bool on = tab == i;
    if (on) {
      dl->AddRectFilled(q, ImVec2(q.x + tw, q.y + h - 1), P.surface800);
      dl->AddRectFilled(ImVec2(q.x + 4, q.y + h - 3), ImVec2(q.x + tw - 4, q.y + h - 1), P.hot);
    }
    if (i == TabStarred) {
      browserui::drawStar(dl, ImVec2(q.x + tw * 0.5f, q.y + h * 0.5f - 1), 5.5f, on ? P.hot : hov ? P.text : P.textDim, on);
    } else {
      float lw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, labels[i]).x;
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(q.x + (tw - lw) * 0.5f), q.y + 5), on ? P.text : hov ? P.text : P.textDim,
                  labels[i]);
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
}

}  // namespace

void drawBrowser(App& a) {
  forgetStaleListings();
  if (!a.showBrowser) return;
  if (!ui::BeginDockWindow("Browser", &a.showBrowser, ImVec2(260, 600))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  loadFolders(a);
  browserui::ensureIndex(a);
  browserui::tickIndex(a);
  if (!ImGui::GetDragDropPayload()) fops.browserDrag.clear();  // K-18
  if (devRowsOn) devRowsLast = std::move(devRowsNow), devRowsNow.clear();
  static char filterBuf[128] = "";
  if (!a.browserSearch.empty()) {
    std::snprintf(filterBuf, sizeof filterBuf, "%s", a.browserSearch.c_str());
    a.browserSearch.clear();
  }
  static bool devInit = false;
  if (!devInit) {  // dev: screenshots
    devInit = true;
    if (const char* t = std::getenv("GRIDLINE_BROWSER_TAB")) tab = std::clamp(std::atoi(t), 0, TabCount - 1);
    // GRIDLINE_BROWSER_DROP="playlist=<file>;mixer=<file>;…" runs drops as if
    // dragged there (targets: rack, playlist, piano, mixer, slot, window, open).
    if (const char* d = std::getenv("GRIDLINE_BROWSER_DROP")) {
      std::string all = d;
      for (size_t pos = 0; pos < all.size();) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string t = item.substr(0, eq), path = item.substr(eq + 1);
        browserui::DropPlace pl;
        const Song& s = a.doc.song();
        if (t == "open") {
          browserui::openFile(a, path);
          continue;
        }
        browse::DropTarget target = browse::DropTarget::Workspace;
        if (t == "rack") target = browse::DropTarget::Rack;
        if (t == "playlist") target = browse::DropTarget::Playlist, pl.track = 1, pl.tick = s.ppq * 4;
        if (t == "piano") target = browse::DropTarget::PianoRoll;
        if (t == "mixer" && s.mixer.size() > 1) target = browse::DropTarget::MixerTrack, pl.mixer = s.mixer[1].id;
        if (t == "slot" && s.mixer.size() > 2) target = browse::DropTarget::MixerSlot, pl.mixer = s.mixer[2].id, pl.slot = 2;
        if (t == "window") {
          target = browse::DropTarget::PluginWindow;
          if (a.fxEditor.first != kNoId) pl.owner = a.fxEditor.first, pl.pluginSlot = a.fxEditor.second;
          else pl.owner = a.settingsChannel;
        }
        browserui::performDrop(a, path, target, pl);
      }
    }
    if (const char* s = std::getenv("GRIDLINE_BROWSER_SELECT")) {
      selPath = s;
      if (browse::kindOf(selPath) == Kind::Audio) {
        browse::FileMeta m;
        if (!browserui::index().lookup(selPath, m) || !m.analysed) browserui::index().analyseNow(selPath);
      }
    }
  }

  // ---- search ----
  ui::BeginToolbar("##brtb");
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - 26;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(w);
    static bool refocus = false;  // after the clear button: typing goes on in the empty field
    if (refocus) ImGui::SetKeyboardFocusHere(), refocus = false;
    ImGui::SetNextItemAllowOverlap();  // the clear button drawn over the field's right end takes its clicks
    ImGui::InputTextWithHint("##filter", i18n::tr("Search  (bpm: key: tag:)"), filterBuf, sizeof filterBuf);
    const ImGuiID filterId = ImGui::GetItemID();
    devNoteRect("brsearch");
    ImGui::PopStyleVar();
    ui::FocusRing();
    ui::Hint("Search plugins, presets and files \xC2\xB7 filters: type:audio|midi|preset|project|effect  tag:kick  bpm:120-130  key:Am  len:<2");
    ui::drawIcon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(p.x + 14, p.y + ImGui::GetFrameHeight() * 0.5f), 12, P.textFaint);
    if (filterBuf[0]) {
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - 22, p.y + 2));
      if (ui::IconButton("clear", ui::Icon::Close, false, 20, "Clear search")) {
        filterBuf[0] = 0;
        if (ImGui::GetActiveID() == filterId) ImGui::ClearActiveID();  // an active field would write its own copy back
        refocus = true;
      }
      devNoteRect("brclear");
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w + 4, p.y));
    if (ui::IconButton("picker", ui::Icon::Plug, browserui::pluginPickerOpen(), ImGui::GetFrameHeight(), "Plugin picker (F8)"))
      browserui::togglePluginPicker();
  }
  ui::EndToolbar();
  tabStrip(a);
  const std::string filter = filterBuf;
  const bool searching = filter.find_first_not_of(' ') != std::string::npos;

  // ---- tree ----
  const float footerH = theme::kToolbarH;
  const bool info = !selPath.empty() && tab != TabPlugins;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##tree", ImVec2(0, -footerH - (info ? kInfoH : 0)), ImGuiChildFlags_AlwaysUseWindowPadding);
  if (searching) {
    if (searchResults(a, filter) == 0) {
      ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
      ui::EmptyState(ImGui::GetWindowDrawList(), ImVec2(mn.x, mn.y + 30), ImVec2(mn.x + sz.x, mn.y + sz.y), "No matches",
                     browserui::index().busy() ? "Still indexing \xE2\x80\x94 more results may appear" : "Try fewer words or filters");
    }
  } else {
    const std::string fl = browserui::flDataDir();
    const fs::path patches = fl.empty() ? fs::path() : fs::path(fl) / "Patches";
    const bool all = tab == TabAll;
    if (all || tab == TabProject) {
      sectionCurrentProject(a);
      sectionRecent(a);
      sectionProjects(a);
    }
    if (all || tab == TabPlugins) {
      sectionPluginDatabase(a);
      sectionPluginPresets(a);
      sectionChannelPresets(a);
      sectionMixerPresets(a);
    }
    if (all || tab == TabSounds) {
      if (!fl.empty()) {
        folderSection(a, "Packs", patches / "Packs", role::kMenu, "No packs");
        folderSection(a, "Impulses", patches / "Impulses", role::kGraph, "No impulses");
        folderSection(a, "Scores", patches / "Scores", role::kSnap, "No scores");
        folderSection(a, "Misc", patches / "Misc", role::kGhost, "Empty");
      }
      folderSection(a, "Clipboard files", browserui::contentDir(a, "Clipboard files"), role::kGhost, "Empty", true);
    }
    if (all || tab == TabSounds || tab == TabProject) {
      // Same folders the recorder / renders use ("<project>/Recorded", config folder while unsaved).
      folderSection(a, "Recorded", recordedDir(a), role::kDelete, "Nothing recorded yet", true);
      folderSection(a, "Rendered", renderedDir(a), role::kSelect, "Nothing rendered yet", true);
      folderSection(a, "Imported", urlimportui::importedDir(a), role::kLoop, "Nothing imported from links yet", true);  // J-192
      folderSection(a, "Stems", stemsDir(a), role::kGraph, "No stems yet", true);  // J-191
      folderSection(a, "Edited audio", editedDir(a), role::kSlip, "No edited audio", true);  // K-17: in PROJECT too
      folderSection(a, "Seltmann loops", seltmannLoopsDir(a), role::kSlice, "No loops made yet", true);
    }
    if (all || tab == TabSounds) {
      folderSection(a, "Sliced audio", browserui::contentDir(a, "Sliced audio"), role::kSlip, "No sliced audio", true);
      sectionUserFolders(a);
    }
    if (tab == TabStarred) sectionStarred(a);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  if (info) infoPanel(a);

  // ---- footer ----
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), P.line);
    const float fh = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(p.x + theme::kS2, p.y + (footerH - fh) * 0.5f));
    if (ui::TextButton("Add folder...", false, 0, "Add a folder of samples or projects to the browser (it is indexed for search)")) {
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.title = "Add a folder to the browser";
      const char* h = gl::platform::homeEnv();
      a.fileDialog.dir = h ? h : ".";
      a.fileDialog.exts = {"/"};  // folder mode
      a.fileDialog.onOk = [&a](const std::string& path) { browserui::addBrowserFolder(a, path); };
    }
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton("Sync", browserui::previewSync(), 0,
                       "Tempo sync: previews (and audio clips dropped on the playlist) follow the song tempo when the file's tempo is known"))
      browserui::previewSync() = !browserui::previewSync();
    ImGui::SameLine(0, theme::kS1);
    if (ui::IconButton("refresh", ui::Icon::Loop, false, fh, "Refresh: re-read folders and index new or changed files")) {
      dirCache.clear();
      browserui::refreshIndex(a);
    }
    const browse::Index& ix = browserui::index();
    if (ix.busy()) {
      ImGui::SameLine(0, theme::kS1);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
      char b[48];
      if (ix.pending() > 0) std::snprintf(b, sizeof b, "%zu/%zu", ix.done(), ix.pending());
      else std::snprintf(b, sizeof b, "%zu", ix.size());
      ImGui::PushFont(a.fontMono, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
      ImGui::PopFont();
      ui::Hint("Indexing sample metadata in the background (length, tempo, key, waveform)");
    }
    if (a.engine.previewing()) {
      ImGui::SameLine(0, theme::kS1);
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - fh - theme::kS2, p.y + (footerH - fh) * 0.5f));
      if (ui::IconButton("stopprev", ui::Icon::Stop, true, fh, "Stop the preview")) a.stopPreview();
    }
  }
  browserui::browserDropTarget(a);  // J-75: folders dragged in from other applications
  trashDialog(a);                   // K-18
  ui::EndDockWindow();
}

namespace browserui {
void addBrowserFolder(App& a, const std::string& dir) {
  loadFolders(a);
  std::error_code ec;
  if (!remotefs::isRemote(dir) && !fs::is_directory(dir, ec)) {
    a.notify(std::string(i18n::tr("Not a folder: ")) + dir, true);
    return;
  }
  if (std::find(folders.begin(), folders.end(), dir) != folders.end()) {
    a.notify(std::string(i18n::tr("Already in the browser: ")) + fs::path(dir).filename().string());
    return;
  }
  folders.push_back(dir);
  saveFolders(a);
  refreshIndex(a);
  a.showBrowser = true;
  a.notify(std::string(i18n::tr("Added to the browser: ")) + fs::path(dir).filename().string());
}

void browserDropTarget(App& a) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!w || !pl || !xdragui::ownsPayload(pl)) return;  // drags from other applications only
  if (ImGui::BeginDragDropTargetCustom(w->Rect(), w->ID)) {
    acceptFileDrop(a, browse::DropTarget::Browser, DropPlace{});
    ImGui::EndDragDropTarget();
  }
}

// K-17 / K-18 dev checks (--dev-input "browser ..."):
//   browser tab <0..4>                the tab (All, Project, Plugins, Sounds, Starred)
//   browser rows [on]                 on: record the drawn rows; else print last frame's rows
//   browser rename <path>             the inline name field on that row (type with "text", then "key enter")
//   browser newfolder <dir>           New folder in <dir> (its name field opens)
//   browser move <path> <dir>         a drop of <path> on folder <dir> inside the browser
//   browser trash <path>              the trash dialog for <path>; "browser trashok": its Move to trash
//   browser refs                      the open project's file references (full paths)
//   browser state                     the name field, its error and why <path> is locked ("browser state <path>")
bool devCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return false;
  const std::string& op = args[0];
  const std::string p1 = args.size() > 1 ? args[1] : std::string(), p2 = args.size() > 2 ? args[2] : std::string();
  if (op == "tab") {
    tab = std::clamp(std::atoi(p1.c_str()), 0, TabCount - 1);
  } else if (op == "rows" && p1 == "on") {
    devRowsOn = true;
  } else if (op == "rows") {
    std::printf("browserrows frame %d: %zu\n", frame, devRowsLast.size());
    for (const auto& r : devRowsLast)
      std::printf("  row \"%s\" x %.1f %.1f y %.1f %.1f path %s\n", r.section.c_str(), static_cast<double>(r.x0), static_cast<double>(r.x1),
                  static_cast<double>(r.y0), static_cast<double>(r.y1), r.path.c_str());
  } else if (op == "rename") {
    startRename(p1);
  } else if (op == "newfolder") {
    newFolderIn(a, p1);
  } else if (op == "move") {
    moveIntoFolder(a, p1, p2);
  } else if (op == "trash") {
    fops.trashAsk = p1, fops.trashOpen = true;
  } else if (op == "trashok") {
    trashNow(a);
    ImGui::ClosePopupsExceptModals();
  } else if (op == "refs") {
    std::printf("browserrefs frame %d:\n", frame);
    for (const auto& f : referencedFiles(a.doc.song())) std::printf("  ref %s\n", f.c_str());
  } else if (op == "state") {
    std::printf("browserstate frame %d: renaming \"%s\" error \"%s\" trash \"%s\" locked \"%s\" undo %zu \"%s\"\n", frame, fops.renaming.c_str(),
                fops.error.c_str(), fops.trashAsk.c_str(), p1.empty() ? "" : lockedWhy(a, p1).c_str(), a.doc.history().position(),
                a.doc.history().undoLabel().c_str());
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

// J-27: View > Project picker — the browser on its current-project tab.
void showProjectTab(App& a) {
  tab = TabProject;
  a.showBrowser = true;
}
}  // namespace browserui

}  // namespace gl

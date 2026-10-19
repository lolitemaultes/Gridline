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
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Presets.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

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
};
std::map<std::string, DirCache> dirCache;

const DirCache& listDir(const fs::path& dir, double now) {
  DirCache& d = dirCache[dir.string()];
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

std::vector<std::string> folders;
bool foldersLoaded = false;

void loadFolders(App& a) {
  if (foldersLoaded) return;
  foldersLoaded = true;
  std::ifstream in(fs::path(a.configDir()) / "browser-folders.txt");
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) folders.push_back(l);
  if (folders.empty()) {
    const char* h = std::getenv("HOME");
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

ImU32 roleCol(uint32_t r, float a = 0.9f) { return theme::litRole(r, a); }

// One browser row: icon, name (ellipsised), optional right-aligned detail.
// Returns true when clicked; `dbl` reports a double-click.
bool browserRow(const char* id, ui::Icon icon, const char* name, const char* detail, float indent, bool* dbl, ImU32 iconCol = 0,
                bool selected = false, bool star = false) {
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
  if (star) {
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
bool sectionHeader(const char* title, int count, bool defaultOpen, uint32_t roleColour = role::kView, ui::Icon icon = ui::Icon::Folder) {
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
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 26), hov ? P.surface700 : theme::withAlpha(P.surface800, 0.6f), theme::kRadiusSm);
  ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(q.x + 9, q.y + 13), 10, open ? P.hot : P.textDim);
  ui::drawIcon(dl, icon, ImVec2(q.x + 25, q.y + 13), 12, roleCol(roleColour));
  const std::string shown = std::string(title).substr(0, std::string(title).find("##"));  // "##" suffix: id only
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
bool treeNode(const std::string& key, const char* label, int count, float indent, uint32_t roleColour = role::kView) {
  ImGuiStorage* st = ImGui::GetStateStorage();
  ImGuiID id = ImGui::GetID(key.c_str());
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
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", text);
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

void fileContextMenu(App& a, const std::string& ps) {
  if (!ImGui::BeginPopupContextItem("##fctx")) return;
  ImGui::TextDisabled("%s", theme::fitText(fs::path(ps).filename().string().c_str(), 260));
  if (ImGui::MenuItem(isStarred(ps) ? "Remove star" : "Star")) toggleStar(ps);
  if (ImGui::MenuItem("Open / add")) browserui::openFile(a, ps);
  if (browse::kindOf(ps) == Kind::Audio) {
    if (ImGui::MenuItem("Preview")) browserui::previewFile(a, ps);
    if (ImGui::MenuItem("Add to playlist as audio clip")) {
      browserui::DropPlace pl;
      pl.tick = a.engine.position() > 0 ? static_cast<Tick>(a.engine.position()) : 0;
      browserui::performDrop(a, ps, browse::DropTarget::Playlist, pl);
    }
    if (ImGui::MenuItem("Load as impulse on the selected insert")) browserui::performDrop(a, ps, browse::DropTarget::MixerTrack, {});
  }
  ImGui::EndPopup();
}

void fileRow(App& a, const fs::path& p, float indent) {
  const std::string ps = p.string();
  const Kind k = browse::kindOf(ps);
  // Presets show their names; files keep their extension.
  const bool named = k == Kind::Preset || k == Kind::Chain || k == Kind::Shape || k == Kind::Fst;
  std::string name = named ? p.stem().string() : p.filename().string();
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
    case Kind::Flp: ui::Hint("Double-click to import this FL Studio project"); break;
    case Kind::Project: ui::Hint("Double-click to open this project"); break;
    case Kind::Fst:
    case Kind::Preset:
      ui::Hint("Double-click: load into the open plugin window (or a new channel / the selected insert) \xC2\xB7 drag onto a plugin window, the rack or a mixer slot");
      break;
    case Kind::Chain: ui::Hint("Double-click: load on the selected mixer track \xC2\xB7 drag onto a mixer strip"); break;
    default: break;
  }
  ImGui::PushID(ps.c_str());
  fileContextMenu(a, ps);
  ImGui::PopID();
  browserui::fileDragSource(ps);
}

void dirTree(App& a, const fs::path& dir, int depth, float baseIndent = 12.0f) {
  if (depth > 12) return;
  const DirCache& d = listDir(dir, a.now);
  const float indent = baseIndent + depth * 14.0f;
  for (const auto& sub : d.dirs) {
    std::string key = sub.string();
    if (treeNode(key, sub.filename().string().c_str(), -1, indent)) dirTree(a, sub, depth + 1, baseIndent);
  }
  for (const auto& f : d.files) fileRow(a, f, indent);
}

// Folder of FL Studio's shipped content (read in place) as a tree node.
void flFolderNode(App& a, const char* label, const fs::path& dir, float indent, uint32_t roleColour) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
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
  browserRow(p.type.c_str(), icon, p.name.c_str(), detail, indent, &dbl, 0, false, fav);
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("GL_PLUGIN", p.type.data(), p.type.size());
    ui::DragPreview(icon, p.name.c_str());
    ImGui::EndDragDropSource();
  }
  ImGui::PushID(p.type.c_str());
  if (ImGui::BeginPopupContextItem("##pctx")) {
    host::PluginPrefs prefs = host::pluginPrefs();
    if (ImGui::MenuItem(fav ? "Remove from favourites" : "Add to favourites")) {
      if (fav) prefs.favourites.erase(p.type);
      else prefs.favourites.insert(p.type);
      host::setPluginPrefs(prefs);
    }
    if (ImGui::MenuItem("Plugin picker", "F8")) browserui::togglePluginPicker();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  ObjectId target = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  if (dbl) {
    if (p.kind == PluginKind::Generator) {
      a.settingsChannel = a.addInstrument(p.type);
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
           (p.kind == PluginKind::Generator ? "(double-click or drag onto the channel rack to add \xC2\xB7 right-click: favourite)"
            : p.kind == PluginKind::Effect  ? "(double-click to insert on the selected mixer track, or drag onto a strip or slot)"
                                            : "(double-click to add to the selected channel's note effects)"));
}

// Built-in plugins of one kind, by category (collapsible).
void builtinPlugins(App& a, PluginKind kind, float indent, const char* keyBase) {
  const host::PluginPrefs prefs = host::pluginPrefs();
  std::map<std::string, std::vector<const PluginInfo*>> byCat;
  for (const auto& p : pluginCatalog())
    if (p.kind == kind) byCat[p.category.empty() ? "Other" : p.category].push_back(&p);
  for (auto& [cat, list] : byCat) {
    std::sort(list.begin(), list.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
    const std::string key = std::string(keyBase) + cat;
    if (!treeNode(key, cat.c_str(), static_cast<int>(list.size()), indent, browserui::categoryRole(cat, static_cast<int>(kind)))) continue;
    for (const PluginInfo* p : list) pluginRow(a, *p, indent + 14, nullptr, prefs.favourites.count(p->type) > 0);
  }
}

void installedPlugins(App& a, float indent) {
  const auto& P = pal();
  const host::ScanStatus ss = host::scanStatus();
  std::vector<const PluginInfo*> list;
  for (const PluginInfo* p : dynamicPlugins()) {
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    list.push_back(p);
  }
  const host::PluginPrefs prefs = host::pluginPrefs();
  auto group = [&prefs](const PluginInfo* p) {
    auto it = prefs.categories.find(p->type);
    return it == prefs.categories.end() ? p->category : it->second;
  };
  if (!treeNode("##installed", "Installed", static_cast<int>(list.size()), indent, role::kMenu)) return;
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + 14);
  if (ss.running) {
    std::string cur = fs::path(ss.current).stem().string();
    ui::Label(("Scanning " + std::to_string(ss.done + 1) + "/" + std::to_string(ss.total) + "  " + cur).c_str());
  } else {
    if (ui::TextButton("Rescan", false, 0, "Look for new or updated plugins (right-click: rescan everything, including failed ones)"))
      host::startScan(host::selfExecutablePath(), false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) host::startScan(host::selfExecutablePath(), true);
    ImGui::SameLine(0, 6);
    if (ui::TextButton("Manage", false, 0, "Plugin manager: folders, formats, failed plugins, sandbox, favourites")) a.showPluginManager = true;
    if (ss.failed > 0) {
      ImGui::SameLine(0, 8);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
      ImGui::TextColored(theme::vec(P.textFaint), "%d failed", ss.failed);
      if (ImGui::IsItemHovered()) {
        std::string tip = "Blacklisted (crashed, hung or failed to load; rescanned when updated):";
        for (const auto& [b, why] : host::blacklist()) tip += "\n" + fs::path(b).filename().string() + ": " + why;
        ui::Tooltip(tip.c_str());
      }
    }
  }
  if (list.empty()) {
    emptyLine("No plugins found", indent + 20);
    return;
  }
  std::map<std::string, std::vector<const PluginInfo*>> byGroup;
  for (const PluginInfo* p : list)
    byGroup[(p->kind == PluginKind::Generator ? "Instruments \xC2\xB7 " : "Effects \xC2\xB7 ") + group(p)].push_back(p);
  for (auto& [g, ps] : byGroup) {
    std::sort(ps.begin(), ps.end(), [](const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; });
    if (!treeNode("##inst" + g, g.c_str(), static_cast<int>(ps.size()), indent + 14, role::kMenu)) continue;
    for (const PluginInfo* p : ps) {
      std::string fmtId, fmtUid;
      const host::Format* fmt = host::splitType(p->type, fmtId, fmtUid) ? host::findFormat(fmtId) : nullptr;
      pluginRow(a, *p, indent + 28, fmt ? fmt->label : "", prefs.favourites.count(p->type) > 0);
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
  if (treeNode("##cp.auto", "Automation", static_cast<int>(s.automations.size()), in, role::kGraph)) {
    for (const auto& ac : s.automations) {
      bool dbl = false;
      const std::string id = "##ac" + std::to_string(ac.id);
      browserRow(id.c_str(), ui::Icon::Draw, ac.name.c_str(), nullptr, in + 14, &dbl, theme::rgb(ac.color));
      if (dbl) openAutomationEditor(a, ac.id);
      ui::Hint("Double-click: edit the automation clip");
    }
  }
  // Samples used by the project's channels (preview, drag elsewhere).
  std::vector<std::string> used;
  for (const auto& c : s.channels)
    if (!c.plugin.data.empty() && browse::kindOf(c.plugin.data) == Kind::Audio &&
        std::find(used.begin(), used.end(), c.plugin.data) == used.end())
      used.push_back(c.plugin.data);
  if (treeNode("##cp.smp", "Samples used", static_cast<int>(used.size()), in, role::kPaint))
    for (const auto& u : used) fileRow(a, u, in + 14);
  // Audio saved with the project (recordings, audio editor renders).
  if (!a.projectPath.empty()) {
    const fs::path audio = fs::path(a.projectPath).parent_path() / "Audio";
    std::error_code ec;
    if (fs::is_directory(audio, ec) && treeNode("##cp.audio", "Project audio", -1, in, role::kDelete)) dirTree(a, audio, 0, in + 14);
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
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (!create) return;
  }
  if (!sectionHeader(title, -1, false, roleColour)) return;
  ui::Hint(dir.string());
  if (create) fs::create_directories(dir, ec);
  const DirCache& d = listDir(dir, a.now);
  if (d.dirs.empty() && d.files.empty()) emptyLine(emptyText);
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
    bool open = sectionHeader(f.filename().string().c_str(), -1, false, role::kView);
    ui::Hint(folders[i] + "   (right-click to remove or refresh)");
    if (ImGui::BeginPopupContextItem("##fmenu")) {
      ImGui::TextDisabled("%s", folders[i].c_str());
      if (ImGui::MenuItem("Refresh")) {
        dirCache.clear();
        browserui::refreshIndex(a);
      }
      if (ImGui::MenuItem("Remove from browser")) {
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
    if (favs.empty()) emptyLine("Right-click a plugin: Add to favourites");
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
    if (browse::matchesPlugin(p, q, prefs.favourites.count(p.type) > 0)) plugs.push_back(&p);
  for (const PluginInfo* p : dynamicPlugins()) {
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    if (browse::matchesPlugin(*p, q, prefs.favourites.count(p->type) > 0)) plugs.push_back(p);
  }
  if (!plugs.empty() && sectionHeader("Plugins##res", static_cast<int>(plugs.size()), true, role::kDraw, ui::Icon::Plug)) {
    for (const PluginInfo* p : plugs) pluginRow(a, *p, 12, p->category.c_str(), prefs.favourites.count(p->type) > 0);
    ImGui::Dummy(ImVec2(0, 4));
  }
  shown += static_cast<int>(plugs.size());
  // Files grouped by kind.
  static const std::pair<Kind, const char*> groups[] = {
      {Kind::Audio, "Samples"},       {Kind::Preset, "Presets"}, {Kind::Fst, "FL Studio presets"}, {Kind::Chain, "Effect chains"},
      {Kind::Midi, "MIDI files"},     {Kind::Score, "Scores"},   {Kind::Project, "Projects"},      {Kind::Flp, "FL Studio projects"},
      {Kind::Shape, "Automation shapes"},
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
  if (ImGui::InputTextWithHint("##addtag", "+ tag", tagBuf, sizeof tagBuf, ImGuiInputTextFlags_EnterReturnsTrue) && tagBuf[0]) {
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
  if (!a.showBrowser) return;
  if (!ui::BeginDockWindow("Browser", &a.showBrowser, ImVec2(260, 600))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  loadFolders(a);
  browserui::ensureIndex(a);
  browserui::tickIndex(a);
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
    ImGui::InputTextWithHint("##filter", "Search  (bpm: key: tag:)", filterBuf, sizeof filterBuf);
    ImGui::PopStyleVar();
    ui::FocusRing();
    ui::Hint("Search plugins, presets and files \xC2\xB7 filters: type:audio|midi|preset|project|effect  tag:kick  bpm:120-130  key:Am  len:<2");
    ui::drawIcon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(p.x + 14, p.y + ImGui::GetFrameHeight() * 0.5f), 12, P.textFaint);
    if (filterBuf[0]) {
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - 22, p.y + 2));
      if (ui::IconButton("clear", ui::Icon::Close, false, 20, "Clear search")) filterBuf[0] = 0;
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
      const char* h = std::getenv("HOME");
      a.fileDialog.dir = h ? h : ".";
      a.fileDialog.exts = {"/"};  // folder mode
      a.fileDialog.onOk = [&a](const std::string& path) {
        if (std::find(folders.begin(), folders.end(), path) == folders.end()) {
          folders.push_back(path);
          saveFolders(a);
          browserui::refreshIndex(a);
        }
      };
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
  ui::EndDockWindow();
}

}  // namespace gl

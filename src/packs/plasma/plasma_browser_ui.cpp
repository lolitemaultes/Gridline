// Gridline — "plasma" pack: the Library overlay (over the stage and the dock).
// Sources: the factory sounds, the user's Gridline preset library for Plasma,
// and the preset library folders found on this computer (indexed in the
// background, plasma_library_dsp.cpp, shown as a folder tree at any depth; a
// root reads "Presets 1", "Presets 2" ... and can be renamed), opened through
// the same path as any preset file.
// Folder click: select it (its presets and its sub-folders'), open it; arrow:
// open / close. Preset click: select, double-click / Enter: open, Up / Down:
// step and open. The search filters the selected folder; the column heads sort.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include "../../app/DevSignal.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../../core/I18n.h"
#include "../../core/Presets.h"
#include "../../host/net/Http.h"
#include <fstream>
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace fs = std::filesystem;

namespace {

struct Entry {
  int kind = 0;  // 0 factory (parameters), 1 factory (data), 2 user library, 3 Serum file
  int index = -1;
  std::string file;
};
// A source's presets: what to open (entries) and what the list shows (rows), same order.
struct List {
  std::vector<Entry> entries;
  std::vector<BrowserRow> rows;
  void add(const std::string& name, const std::string& folder, const char* source, Entry e) {
    rows.push_back({name, folder, source});
    entries.push_back(std::move(e));
  }
};

constexpr float kRowH = M(28);

// a caps section head in the Library's folder column
void libCaption(const Ctx& c, float x, float y, const char* s) { capsText(c, x, y + M(8) - T(9.5f) * 0.62f, s, c.P.textFaint, T(9.5f), M(1.2f)); }

List scan(const App& a, const std::string& src) {
  List out;
  if (src == "factory") {
    if (const PluginInfo* in = findPlugin(kType))
      for (int i = 0; i < static_cast<int>(in->presets.size()); ++i) out.add(in->presets[static_cast<size_t>(i)].name, "Factory", "Factory", {0, i, ""});
    const auto& dp = factoryDataPresets();
    for (int i = 0; i < static_cast<int>(dp.size()); ++i) out.add(dp[static_cast<size_t>(i)].name, "Factory", "Factory", {1, i, ""});
  } else if (src == "user") {
    for (const auto& e : listPresets(presetRoot(a.configDir()), kType)) out.add(e.name, e.folder.empty() ? "User" : e.folder, "Library", {2, -1, e.file});
  }
  return out;
}

std::string libraryCache(const App& a) { return (fs::u8path(a.configDir()) / "plasma-library.json").u8string(); }

// Serum library scopes: "serum:" (all), "serum:<root>", "serum:<root>\n<folder path>" (and below).
constexpr const char* kSerum = "serum:";
bool isSerumScope(const std::string& key) { return key.compare(0, 6, kSerum) == 0; }

// The shown names of the library roots: "Presets n" by order, or the user's own name
// (kept in plasma-library-names.txt in the config folder: one "path<TAB>name" per line).
std::map<std::string, std::string>& rootNames(const App& a) {
  static std::map<std::string, std::string> names;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    std::ifstream in(fs::u8path(a.configDir()) / "plasma-library-names.txt");
    std::string line;
    while (std::getline(in, line)) {
      const size_t t = line.find('\t');
      if (t != std::string::npos && t + 1 < line.size()) names[line.substr(0, t)] = line.substr(t + 1);
    }
  }
  return names;
}
void saveRootNames(const App& a) {
  std::ofstream out(fs::u8path(a.configDir()) / "plasma-library-names.txt", std::ios::trunc);
  for (const auto& [path, name] : rootNames(a)) out << path << '\t' << name << '\n';
}
std::string rootPath(const std::string& key) {  // "serum:<root>[\n<folder>]" -> <root>
  const std::string r = key.substr(std::min<size_t>(6, key.size()));
  return r.substr(0, r.find('\n'));
}
// Root labels in tree order.
void nameRoots(const App& a, std::vector<LibraryFolder>& tree) {
  int n = 0;
  for (auto& f : tree)
    if (f.depth == 0) {
      ++n;
      const auto& names = rootNames(a);
      const auto it = names.find(rootPath(f.key));
      f.label = it != names.end() ? it->second : "Presets " + std::to_string(n);
    }
}

bool isFxp(const std::string& file) {
  std::string e = fs::u8path(file).extension().u8string();
  for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return e == ".fxp";
}

List libraryList(const LibrarySnapshot& snap, const std::string& key) {
  List out;
  const auto idx = libraryPresetsIn(snap, key);
  out.rows.reserve(idx.size());
  out.entries.reserve(idx.size());
  std::map<std::string, std::string> rootLabel;
  for (int i : idx) {
    const LibraryPreset& p = snap.presets[static_cast<size_t>(i)];
    std::string folder = p.bank;
    if (folder.empty()) {
      auto it = rootLabel.find(p.root);
      if (it == rootLabel.end()) it = rootLabel.emplace(p.root, "Library").first;
      folder = it->second;
    }
    out.add(p.name, folder, isFxp(p.file) ? "Imported (older)" : "Imported (newer)", {3, -1, p.file});
  }
  return out;
}

std::vector<std::pair<int, float>> valuesOf(const PluginState& st) {
  std::vector<std::pair<int, float>> v;
  for (size_t i = 0; i < st.params.size(); ++i) v.emplace_back(static_cast<int>(i), static_cast<float>(st.params[i]));
  return v;
}

bool load(Ctx& c, const Entry& e, const std::string& name, std::string& msg) {
  msg.clear();
  if (e.kind == 0) {
    if (const PluginInfo* in = findPlugin(kType)) applyPreset(c, in->presets[static_cast<size_t>(e.index)].values, "", name);
    msg = "Factory preset";
    return true;
  }
  if (e.kind == 1) {
    const auto& p = factoryDataPresets()[static_cast<size_t>(e.index)];
    applyPreset(c, p.values, p.data, p.name);
    msg = "Factory preset";
    return true;
  }
  if (e.kind == 2) {
    PluginState st;
    if (loadPreset(e.file, kType, st)) {
      applyPreset(c, valuesOf(st), st.data, name);
      msg = "Preset library: " + e.file;
      return true;
    }
    msg = "Could not read " + e.file;
    return false;
  }
  // Serum presets: the same Open path as the file dialog and drops
  if (openPreset(c, e.file)) {
    Model m = defaultModel();
    if (const PluginState* st = c.t.state(c.a.doc.song())) modelFromData(st->data, m);
    msg = openReportText(m);
    return true;
  }
  msg = "Could not open " + e.file;
  return false;
}

bool gDropUser = false;  // a save went to the user library: its list is read again

std::string entryId(const Entry& e) { return std::to_string(e.kind) + "|" + std::to_string(e.index) + "|" + e.file; }

// Folder-tree chevron: pointing right (closed) or down (open).
void chevron(const Ctx& c, float x, float y, bool open, ImU32 col) {
  const ImVec2 m = c.p(x, y);
  const float s = c.s(3.2f);
  if (open)
    c.dl->AddTriangleFilled(ImVec2(m.x - s, m.y - s * 0.6f), ImVec2(m.x + s, m.y - s * 0.6f), ImVec2(m.x, m.y + s * 0.7f), col);
  else
    c.dl->AddTriangleFilled(ImVec2(m.x - s * 0.6f, m.y - s), ImVec2(m.x - s * 0.6f, m.y + s), ImVec2(m.x + s * 0.7f, m.y), col);
}

}  // namespace

void saveAsButton(Ctx& c, const char* id, float x, float y, float w, float h, bool quiet) {
  ImGui::PushID(id);
    static char saveName[96] = "";
    if (quiet ? btnLook(c, id, x, y, w, h, "Save as", true, false, T(12)) : button(c, id, x, y, w, h, "Save as\xE2\x80\xA6", false, 11.0f)) {
      std::snprintf(saveName, sizeof saveName, "%s", c.m.presetName.c_str());
      ImGui::OpenPopup("##psave");
    }
    if (ImGui::BeginPopup("##psave")) {
      ImGui::TextUnformatted(i18n::tr("Save to the preset library (folders with /)"));
      ImGui::SetNextItemWidth(c.s(240));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##pn", saveName, sizeof saveName, ImGuiInputTextFlags_EnterReturnsTrue);
      if ((enter || ImGui::Button(i18n::tr("Save"))) && saveName[0]) {
        std::string rel = saveName, clean;
        // each folder part made safe
        size_t s0 = 0;
        while (s0 <= rel.size()) {
          const size_t s1 = rel.find('/', s0);
          const std::string part = safePresetName(rel.substr(s0, s1 == std::string::npos ? std::string::npos : s1 - s0));
          if (!part.empty()) clean += (clean.empty() ? "" : "/") + part;
          if (s1 == std::string::npos) break;
          s0 = s1 + 1;
        }
        if (!clean.empty()) {
          PluginState st = c.st;
          Model m = c.m;
          m.presetName = fs::path(clean).filename().string();
          st.data = modelToData(m);
          const bool ok = savePreset(presetRoot(c.a.configDir()), st, clean, "Plasma");
          c.a.notify(ok ? "Saved the preset" : "Could not save the preset", !ok);
          gDropUser = true;
          if (ok) {
            c.m.presetName = m.presetName;
            commit(c, "Name preset");
          }
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  ImGui::PopID();
}

// The sound before / after the current one in the Library's selected folder (the top bar arrows).
bool libraryStep(Ctx& c, int dir) {
  const std::string scope = c.u.browserDir.empty() ? std::string("factory") : c.u.browserDir;
  List l;
  if (isSerumScope(scope)) l = libraryList(*librarySnapshot(), scope);
  else l = scan(c.a, scope);
  if (l.rows.empty()) return false;
  const int n = static_cast<int>(l.rows.size());
  int cur = -1;
  for (int i = 0; i < n; ++i)
    if (l.rows[static_cast<size_t>(i)].name == c.m.presetName) {
      cur = i;
      break;
    }
  const int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
  std::string msg;
  return load(c, l.entries[static_cast<size_t>(next)], l.rows[static_cast<size_t>(next)].name, msg);
}

void presetBrowser(Ctx& c) {
  Ui& u = c.u;
  if (u.browserDir.empty()) u.browserDir = "factory";
  // lists per source (rebuilt on REFRESH / a new library index), the shown rows (rebuilt when the
  // list, the search or the sort changes), the selected preset (by identity, kept across both)
  static std::map<std::string, List> cache;
  static std::vector<LibraryFolder> tree;
  static std::map<std::string, bool> openSet;  // folders opened / closed by the user (else the roots are open)
  static uint64_t libGen = ~uint64_t(0);
  static std::string selId, infoText;
  static int sortCol = -1;
  static bool sortDesc = false;
  static std::vector<int> shown;
  static std::string shownScope, shownQuery;
  static int shownSort = -2, loads = 0;
  static bool shownDesc = false, shownDirty = true;
  static float treeScroll = 0;
  static bool dropAll = false;  // Read again / Save as: lists read again next frame
  if (dropAll || gDropUser) {
    if (dropAll) cache.clear();
    else cache.erase("user");
    dropAll = gDropUser = false;
    shownDirty = true;
  }
  const std::string libCache = libraryCache(c.a);
  libraryRefresh(libCache);
  const auto snap = librarySnapshot();
  if (snap->generation != libGen) {
    libGen = snap->generation;
    tree = libraryFolderTree(*snap);
    nameRoots(c.a, tree);
    for (auto it = cache.begin(); it != cache.end();) it = isSerumScope(it->first) ? cache.erase(it) : std::next(it);
    shownDirty = true;
  }
  if (snap->generation > 0 && !snap->indexing) devsig::reach("plasma-library-ready");
  const std::string scope = u.browserDir;  // the search covers the selected folder
  if (!cache.count(scope)) {
    cache[scope] = isSerumScope(scope) ? libraryList(*snap, scope) : scan(c.a, scope);
    shownDirty = true;
  }
  const List& all = cache[scope];
  const std::string query = u.browserFilter;
  bool scrollTop = false;
  static std::string selFor;  // the selected preset's row in `shown`, found again when either changes
  static int selRow = -1;
  if (shownDirty || shownScope != scope || shownQuery != query || shownSort != sortCol || shownDesc != sortDesc) {
    scrollTop = shownScope != scope || shownQuery != query;
    shown = browserFilter(all.rows, query, sortCol, sortDesc);
    shownScope = scope, shownQuery = query, shownSort = sortCol, shownDesc = sortDesc, shownDirty = false;
    selFor = "\n";  // (no entry id has a line break)
  }
  if (selId.empty() && !c.m.presetName.empty())  // the open sound is the chosen one at first
    for (size_t r = 0; r < shown.size(); ++r)
      if (all.rows[static_cast<size_t>(shown[r])].name == c.m.presetName) {
        selId = entryId(all.entries[static_cast<size_t>(shown[r])]);
        break;
      }
  if (selFor != selId) {
    selFor = selId, selRow = -1;
    if (!selId.empty())
      for (size_t r = 0; r < shown.size(); ++r)
        if (entryId(all.entries[static_cast<size_t>(shown[r])]) == selId) {
          selRow = static_cast<int>(r);
          break;
        }
  }
  int sel = selRow;
  // the Library frame's parts (libraryPage draws the frame): folders, the list, the sound
  const float SX = kLibX0, kSideW = kLibSideW, kTopY = kLibBodyY, kStripY = kLibY1, kInfoX = kLibInfoX;
  pui::DevPoints devFolders;  // dev / tests: one point per folder row (k: -2 factory, -1 user, -3 all, else tree index)
  devFolders.x0 = c.p(SX, 0).x, devFolders.x1 = c.p(SX + kSideW, 0).x, devFolders.y0 = c.p(0, kTopY).y, devFolders.y1 = c.p(0, kStripY).y;
  const auto devRow = [&](float y, double code) {
    const double fx = 0.5, fv = (static_cast<double>(devFolders.y1) - c.p(0, y + kRowH * 0.5f).y) / std::max(1.0f, devFolders.y1 - devFolders.y0);
    devFolders.pts.push_back({fx, fv, code});
  };
  const int selCode = u.browserDir == "factory" ? -2 : u.browserDir == "user" ? -1 : u.browserDir == kSerum ? -3 : -4;
  float y = kTopY + M(16);
  // a folder row: highlight, chevron (when it has sub-folders), label, count; returns 1 on a
  // click on the row, 2 on a click on the chevron
  auto row = [&](const char* id, const std::string& label, const std::string& key, int depth, int count, int kids, bool open) {
    int hit = 0;
    const bool on = u.browserDir == key;
    const float ax = SX + M(24) + depth * M(14);  // chevron centre
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(c.p(SX + M(10), y));
    if (ImGui::InvisibleButton("##f", c.F.sz(kSideW - M(20), kRowH))) {
      const float mx = ImGui::GetIO().MousePos.x;
      hit = kids && mx < c.p(ax + 7, 0).x ? 2 : 1;
    }
    const bool hv = ImGui::IsItemHovered();
    if (hv && isSerumScope(key) && depth == 0 && key.size() > 6) c.a.setHint("Right-click: rename, or show the folder in the file manager");
    if (on || hv) panelRect(c, SX + M(10), y, kSideW - M(20), kRowH - 1, on ? c.P.panelHi : theme::withAlpha(c.P.panelHi, 0.5f), M(4));
    if (on) c.dl->AddRectFilled(c.p(SX + M(10), y + M(3)), c.p(SX + M(12.5f), y + kRowH - M(4)), c.P.accent, c.s(M(1)));
    if (kids) chevron(c, ax, y + kRowH * 0.5f - 0.5f, open, on || hv ? c.P.text : c.P.textFaint);
    const float lx = ax + M(12);
    char cnt[24] = "";
    if (count >= 0) std::snprintf(cnt, sizeof cnt, "%d", count);
    float cw = 0;
    if (cnt[0]) {
      MonoText mono;
      cw = textW(c, cnt, T(10), false) + M(6);
    }
    // the label is clipped before the count
    const ImVec2 c0 = c.p(lx, y), c1 = c.p(SX + kSideW - M(18) - cw, y + kRowH);
    c.dl->PushClipRect(c0, c1, true);
    text(c, lx, y + kRowH * 0.5f - T(12) * 0.68f, label.c_str(), on ? c.P.text : c.P.textDim, depth ? T(11.5f) : T(12), false);
    c.dl->PopClipRect();
    if (cnt[0]) {
      MonoText mono;
      textR(c, SX + kSideW - M(18), y + kRowH * 0.5f - T(10) * 0.68f, cnt, c.P.textFaint, T(10), false);
    }
    ImGui::PopID();
    return hit;
  };
  libCaption(c, SX + M(18), y, "Factory");
  y += M(24);
  devRow(y, -2);
  if (row("fac", "All sounds", "factory", 0, -1, 0, false)) u.browserDir = "factory";
  y += kRowH + M(14);
  libCaption(c, SX + M(18), y, "Your sounds");
  y += M(24);
  devRow(y, -1);
  if (row("usr", "Saved sounds", "user", 0, -1, 0, false)) u.browserDir = "user";
  y += kRowH + M(14);
  libCaption(c, SX + M(18), y, snap->indexing ? "Installed library (looking)" : "Installed library");
  y += M(24);
  static char addPath[512] = "";
  auto bottomButtons = [&](float by) {
    const float bw = (kSideW - M(36)) * 0.5f;
    if (btnLook(c, "open", SX + M(14), by, bw, M(28), "Open a file", false, false, T(11.5f))) openPresetDialog(c);
    if (ImGui::IsItemHovered()) c.a.setHint("Open a Plasma preset or a compatible preset file");
    if (btnLook(c, "addf", SX + M(22) + bw, by, bw, M(28), "Add a folder", false, false, T(11.5f))) addPath[0] = 0, ImGui::OpenPopup("##addfolder");
    if (ImGui::IsItemHovered()) c.a.setHint("Add a preset folder (its wavetables and samples are found there too)");
    if (ImGui::BeginPopup("##addfolder")) {
      ImGui::TextUnformatted(i18n::tr("Preset folder"));
      ImGui::SetNextItemWidth(c.s(320));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##fp", addPath, sizeof addPath, ImGuiInputTextFlags_EnterReturnsTrue);
      std::error_code ec;
      if ((enter || ImGui::Button(i18n::tr("Add"))) && addPath[0]) {
        if (fs::is_directory(fs::u8path(addPath), ec)) {
          auto extra = extraSerumFolders();
          if (std::find(extra.begin(), extra.end(), std::string(addPath)) == extra.end()) extra.push_back(addPath);
          setExtraSerumFolders(extra);
          librarySaveFolders(libCache);
          libraryRefresh(libCache, true);
        } else {
          c.a.notify(std::string(i18n::tr("Not a folder")) + ": " + addPath, true);
        }
        ImGui::CloseCurrentPopup();
      }
      const auto extra = extraSerumFolders();
      if (!extra.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(i18n::tr("Added folders (click to remove)"));
        for (const auto& f : extra)
          if (ImGui::Selectable(f.c_str())) {
            std::vector<std::string> keep;
            for (const auto& g : extra)
              if (g != f) keep.push_back(g);
            setExtraSerumFolders(keep);
            librarySaveFolders(libCache);
            libraryRefresh(libCache, true);
          }
      }
      ImGui::EndPopup();
    }
  };
  if (tree.empty()) {
    text(c, SX + M(18), y + M(2), snap->indexing ? "Looking for preset folders\xE2\x80\xA6" : "No preset folders found. Add one below", c.P.textFaint, T(11.5f), false);
    y += M(24);
    bottomButtons(y);
  } else {
    // The folder tree: roots open unless closed, other folders closed unless opened. It scrolls
    // with the wheel and its thumb; rows are hit-tested inside the tree's area only (ImGui clip
    // rect), so a row half scrolled out never takes a click meant for the buttons below.
    std::vector<char> open(tree.size(), 0);
    for (size_t i = 0; i < tree.size(); ++i) {
      const auto it = openSet.find(tree[i].key);
      open[i] = it != openSet.end() ? it->second : tree[i].depth == 0;
    }
    const std::vector<int> vis = libraryVisibleFolders(tree, open);
    const float top = y, bottom = kStripY - M(52), viewH = bottom - top;
    const float total = static_cast<float>(vis.size() + 1) * kRowH;
    const ImVec2 a0 = c.p(SX, top), a1 = c.p(SX + kSideW, bottom);
    // a tree that scrolls owns the wheel while the pointer is over it (one owner for the whole
    // area: the window around it stays put)
    const ImGuiID wheelId = ImGui::GetID("##ftree");
    if (total > viewH && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a0, a1)) {
      if (ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, wheelId)) {
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, wheelId);
        treeScroll -= ImGui::GetIO().MouseWheel * kRowH * 3;
      }
    }
    treeScroll = std::clamp(treeScroll, 0.f, std::max(0.f, total - viewH));
    ImGui::PushClipRect(a0, a1, true);
    y = top - treeScroll;
    const auto visible = [&] { return y + kRowH > top && y < bottom; };
    if (visible()) {
      devRow(y, -3);
      if (row("sall", "Found on this computer", kSerum, 0, static_cast<int>(snap->presets.size()), 0, false)) u.browserDir = kSerum;
    }
    y += kRowH;
    for (int i : vis) {
      if (!visible()) {
        y += kRowH;
        continue;
      }
      const LibraryFolder& f = tree[static_cast<size_t>(i)];
      devRow(y, i);
      ImGui::PushID(i);
      const int hit = row("sf", f.label, f.key, f.depth, f.count, f.hasChildren, open[static_cast<size_t>(i)] != 0);
      if (f.depth == 0) {
        static char rn[96] = "";
        ImGui::PushID("sf");
        if (ImGui::BeginPopupContextItem("##root")) {
          if (ImGui::IsWindowAppearing()) std::snprintf(rn, sizeof rn, "%s", f.label.c_str());
          ImGui::TextUnformatted(i18n::tr("Name"));
          ImGui::SetNextItemWidth(c.s(200));
          if (ImGui::InputText("##rn", rn, sizeof rn, ImGuiInputTextFlags_EnterReturnsTrue) && rn[0]) {
            rootNames(c.a)[rootPath(f.key)] = rn;
            saveRootNames(c.a);
            nameRoots(c.a, tree);
            ImGui::CloseCurrentPopup();
          }
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::tr("Show in file manager"))) gl::http::reveal(rootPath(f.key));
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
      ImGui::PopID();
      if (hit == 2) {
        openSet[f.key] = !open[static_cast<size_t>(i)];
      } else if (hit == 1) {
        // a click selects the folder and opens it; a click on the selected open folder closes it
        if (f.hasChildren) openSet[f.key] = u.browserDir == f.key ? !open[static_cast<size_t>(i)] : true;
        u.browserDir = f.key;
      }
      y += kRowH;
    }
    ImGui::PopClipRect();
    // scroll thumb (drag), at the right edge, clear of the rows
    if (total > viewH) {
      const float th = std::max(24.f, viewH * viewH / total), ty = top + (viewH - th) * treeScroll / (total - viewH);
      ImGui::SetCursorScreenPos(c.p(SX + kSideW - 6, top));
      ImGui::InvisibleButton("##fthumb", c.F.sz(6, viewH));
      const bool act = ImGui::IsItemActive(), hv = ImGui::IsItemHovered();
      if (act && ImGui::GetIO().MouseDelta.y != 0) treeScroll += ImGui::GetIO().MouseDelta.y / c.s(1) * (total - viewH) / std::max(1.f, viewH - th);
      if (ImGui::IsItemClicked()) {  // a click off the thumb jumps there
        const float my = (ImGui::GetIO().MousePos.y - a0.y) / c.s(1);
        if (my < ty - top || my > ty - top + th) treeScroll = std::clamp((my - th * 0.5f) / std::max(1.f, viewH - th), 0.f, 1.f) * (total - viewH);
      }
      panelRect(c, SX + kSideW - 5, ty, 3, th, act || hv ? c.P.lineHi : c.P.line, 1.5f);
    }
    bottomButtons(kStripY - M(42));
  }
  pui::devPointsSet("plasma.folders", devFolders);

  // ---- the count and Read again, over the list (the search is in the Library's header)
  {
    char b[48];
    std::snprintf(b, sizeof b, i18n::tr("%d sounds"), static_cast<int>(shown.size()));
    const float ty = kTopY + M(14);
    {
      MonoText mono;
      text(c, SX + kSideW + M(16), ty + M(13) - T(11) * 0.68f, b, c.P.textFaint, T(11), false);
    }
    if (btnLook(c, "refresh", kInfoX - M(16) - M(100), ty, M(100), M(26), "Read again", false, false, T(11.5f))) {
      dropAll = true;
      libraryRefresh(libCache, true);
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Read the preset folders again");
  }
  // ---- list: column heads (click: sort up, again: down, again: list order), rows
  const float lx = SX + kSideW + M(16), ly = kTopY + M(52), lw = kInfoX - M(16) - lx, lh = kStripY - M(14) - ly;
  c.dl->AddRectFilled(c.p(lx, ly), c.p(lx + lw, ly + lh), theme::mix(c.P.panel, c.P.frame, 0.3f), c.s(M(5)));
  c.dl->AddRect(c.p(lx, ly), c.p(lx + lw, ly + lh), c.P.line, c.s(M(5)));
  {
    const float colX[3] = {lx + M(50), lx + lw * 0.42f, lx + lw * 0.6f};
    const float colW[3] = {lw * 0.42f - M(50), lw * 0.18f, lw * 0.4f};
    const char* names[3] = {"Name", "Folder", "From"};
    for (int k = 0; k < 3; ++k) {
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(c.p(colX[k] - 6, ly));
      if (ImGui::InvisibleButton("##sort", c.F.sz(colW[k], M(28)))) {
        if (sortCol != k) sortCol = k, sortDesc = false;
        else if (!sortDesc) sortDesc = true;
        else sortCol = -1, sortDesc = false;
      }
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.a.setHint("Sort by this column (click again: reverse, a third time: list order)");
      capsText(c, colX[k], ly + M(14) - T(9.5f) * 0.62f, names[k], sortCol == k ? c.P.text : (hv ? c.P.textDim : c.P.textFaint), T(9.5f), M(0.8f));
      if (sortCol == k) iconArrow(c.dl, c.p(colX[k] + capsW(c, names[k], T(9.5f), M(0.8f)) + M(8), ly + M(14)), c.s(M(4)), sortDesc ? 3 : 2, c.P.accent);
      ImGui::PopID();
    }
  }
  hline(c, lx, lx + lw, ly + M(28), c.P.line);
  ImGui::SetCursorScreenPos(c.p(lx, ly + M(29)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  bool loadNow = false;
  int moveTo = -1;
  pui::DevPoints devRows;  // dev / tests: the visible preset rows (k = row in the list)
  if (ImGui::BeginChild("##plist", c.F.sz(lw, lh - M(30)), false)) {
    if (scrollTop) ImGui::SetScrollY(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = c.s(M(42));
    const ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    devRows.x0 = w0.x, devRows.y0 = w0.y, devRows.x1 = w0.x + ws.x, devRows.y1 = w0.y + ws.y;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(shown.size()), rowH);
    while (clip.Step()) {
      for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
        const size_t ei = static_cast<size_t>(shown[static_cast<size_t>(r)]);
        const BrowserRow& e = all.rows[ei];
        ImGui::PushID(r);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##row", ImVec2(c.s(lw), rowH))) sel = r, selId = entryId(all.entries[ei]);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) sel = r, selId = entryId(all.entries[ei]), loadNow = true;
        const bool on = sel == r, hv = ImGui::IsItemHovered();
        if (p0.y + rowH * 0.5f > devRows.y0 && p0.y + rowH * 0.5f < devRows.y1)
          devRows.pts.push_back({0.3, (static_cast<double>(devRows.y1) - (p0.y + rowH * 0.5f)) / std::max(1.0f, devRows.y1 - devRows.y0), static_cast<double>(r)});
        if (on) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), theme::mix(c.P.panel, c.P.accent, 0.14f));
        else if (hv) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), c.P.panelHi);
        if (on) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(M(2.5f)), p0.y + rowH), c.P.accent);
        dl->AddLine(ImVec2(p0.x, p0.y + rowH - 1), ImVec2(p0.x + c.s(lw), p0.y + rowH - 1), theme::withAlpha(c.P.line, 0.7f));
        {  // a round play mark at the left (double-click or Enter opens the sound)
          const ImVec2 pc(p0.x + c.s(M(23)), p0.y + rowH * 0.5f);
          dl->AddCircle(pc, c.s(M(10)), c.P.line, 20, std::max(1.f, c.s(M(1))));
          const float tr = c.s(M(3));
          dl->AddTriangleFilled(ImVec2(pc.x - tr * 0.6f, pc.y - tr), ImVec2(pc.x - tr * 0.6f, pc.y + tr), ImVec2(pc.x + tr, pc.y), on ? c.P.text : c.P.textFaint);
        }
        ImFont* f = c.a.fontRegular;
        const float fsz = font(c, T(12));
        const float ty = p0.y + (rowH - fsz) * 0.5f;
        // each column clipped to its width
        const float cx[3] = {p0.x + c.s(M(50)), p0.x + c.s(lw * 0.42f), p0.x + c.s(lw * 0.6f)};
        const std::string* cols[3] = {&e.name, &e.folder, &e.source};
        const ImU32 colC[3] = {c.P.text, c.P.textDim, c.P.textDim};
        for (int k = 0; k < 3; ++k) {
          const float x1 = k < 2 ? cx[k + 1] - c.s(8) : p0.x + c.s(lw);
          dl->PushClipRect(ImVec2(cx[k], p0.y), ImVec2(x1, p0.y + rowH), true);
          if (k == 1) {  // the folder as a tag
            const float ts = fsz * 0.86f, tw = f->CalcTextSizeA(ts, 1e9f, 0, cols[k]->c_str()).x;
            dl->AddRectFilled(ImVec2(cx[k], p0.y + rowH * 0.5f - c.s(M(9))), ImVec2(cx[k] + tw + c.s(M(14)), p0.y + rowH * 0.5f + c.s(M(9))),
                              theme::mix(c.P.panelHi, c.P.accent, 0.14f), c.s(M(4)));
            dl->AddText(f, ts, ImVec2(cx[k] + c.s(M(7)), p0.y + (rowH - ts) * 0.5f), colC[k], cols[k]->c_str());
          } else {
            dl->AddText(f, fsz, ImVec2(cx[k], ty), colC[k], cols[k]->c_str());
          }
          dl->PopClipRect();
        }
        ImGui::PopID();
      }
    }
    if (ImGui::IsWindowFocused() && !shown.empty()) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) moveTo = std::min(static_cast<int>(shown.size()) - 1, sel + 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) moveTo = std::max(0, sel - 1);
      if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && sel >= 0) loadNow = true;
    }
    if (moveTo >= 0) {
      sel = moveTo, loadNow = true;
      selId = entryId(all.entries[static_cast<size_t>(shown[static_cast<size_t>(sel)])]);
      const float yr = static_cast<float>(sel) * rowH;
      if (yr < ImGui::GetScrollY()) ImGui::SetScrollY(yr);
      if (yr + rowH > ImGui::GetScrollY() + ImGui::GetWindowHeight()) ImGui::SetScrollY(yr + rowH - ImGui::GetWindowHeight());
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  pui::devPointsSet("plasma.presets", devRows);
  if (shown.empty())
    textC(c, lx + lw * 0.5f, ly + M(80),
          !query.empty()                               ? "No presets match the search"
          : u.browserDir == "user"                     ? "No saved sounds yet: Save as stores the current sound here"
          : isSerumScope(scope) && snap->indexing      ? "Scanning preset folders\xE2\x80\xA6"
                                                       : "No presets here",
          c.P.textFaint, T(12), false);
  if (loadNow && sel >= 0 && sel < static_cast<int>(shown.size())) {
    const size_t ei = static_cast<size_t>(shown[static_cast<size_t>(sel)]);
    if (load(c, all.entries[ei], all.rows[ei].name, infoText)) ++loads;
  }

  // ---- the sound: its name and where it is from, what it sounds like, its notes; open it
  {
    const float x = kInfoX + M(16), w = kLibX1 - M(16) - x, iy = kTopY + M(16);
    const bool has = sel >= 0 && sel < static_cast<int>(shown.size());
    const size_t ei = has ? static_cast<size_t>(shown[static_cast<size_t>(sel)]) : 0;
    if (has) {
      const BrowserRow& e = all.rows[ei];
      c.dl->PushClipRect(c.p(x, iy), c.p(x + w, iy + M(24)), true);
      text(c, x, iy + M(10) - T(15) * 0.68f, e.name.c_str(), c.P.text, T(15), true);
      const float nw = textW(c, e.name.c_str(), T(15), true);
      text(c, x + nw + M(8), iy + M(11) - T(11) * 0.68f, (e.source + ", " + e.folder).c_str(), c.P.textFaint, T(11), false);
      c.dl->PopClipRect();
    } else {
      text(c, x, iy + M(10) - T(12) * 0.68f, "Choose a sound", c.P.textFaint, T(12), false);
    }
    // what it sounds like: the open sound's first layer (Up / Down open the sounds as you go)
    const float wy = iy + M(24), wh = M(200);
    wellLook(c, x, wy, w, wh);
    gridLook(c, x, wy, w, wh, 8, 4, true);
    capsText(c, x + M(9), wy + M(8), "What it sounds like", c.P.textFaint, T(9), M(1));
    if (has && all.rows[ei].name == c.m.presetName) {
      for (int o = 0; o < 3; ++o)
        if (val(c, oscP(o, O_Enable)) >= 0.5) {
          layerPicture(c, o, x + M(4), wy + M(22), w - M(8), wh - M(26));
          break;
        }
    } else if (has) {
      textC(c, x + w * 0.5f, wy + wh * 0.5f - T(11) * 0.68f, "Double-click or Enter to hear it", c.P.textFaint, T(11), false);
    }
    float ty = wy + wh + M(12);
    if (has) {  // its folder and where it is from as tags
      float tx = x;
      for (const std::string* t : {&all.rows[ei].folder, &all.rows[ei].source}) {
        const float tw = textW(c, t->c_str(), T(10.5f), false) + M(18);
        if (t->empty() || tx + tw > x + w) continue;
        c.dl->AddRect(c.p(tx, ty), c.p(tx + tw, ty + M(20)), c.P.line, c.s(M(10)));
        text(c, tx + M(9), ty + M(10) - T(10.5f) * 0.68f, t->c_str(), c.P.textDim, T(10.5f), false);
        tx += tw + M(6);
      }
      ty += M(30);
    }
    if (!infoText.empty()) {
      ImGui::SetCursorScreenPos(c.p(x, ty));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
      if (ImGui::BeginChild("##pinfo", c.F.sz(w, std::max(M(20), kStripY - M(70) - ty)), false)) {
        ImGui::PushStyleColor(ImGuiCol_Text, c.P.textDim);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(infoText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    const float by = kStripY - M(16) - M(30), bw = (w - M(8)) * 0.5f;
    static bool held = false;
    btnLook(c, "note", x, by, bw, M(30), "Play a note", false, false, T(12));
    if (ImGui::IsItemActivated()) previewKey(c, 60, true), held = true;
    if (held && !ImGui::IsItemActive()) previewKey(c, 60, false), held = false;
    if (ImGui::IsItemHovered()) c.a.setHint("Hold to hear the open sound");
    if (btnLook(c, "load", x + bw + M(8), by, bw, M(30), "Load sound", false, true, T(12)) && has && load(c, all.entries[ei], all.rows[ei].name, infoText)) ++loads;
    if (ImGui::IsItemHovered()) c.a.setHint("Open this sound (or double-click it, or press Enter; Up and Down go through the list)");
  }
  // dev / tests: the browser's state (pt 0: x = presets listed, v = selected row, k = selected
  // folder: -2 factory, -1 user, -3 all Serum presets, else its tree index; pt 1: x = presets opened)
  {
    int code = selCode;
    if (code == -4)
      for (size_t i = 0; i < tree.size(); ++i)
        if (tree[i].key == u.browserDir) code = static_cast<int>(i);
    pui::DevPoints d;
    d.pts.push_back({static_cast<double>(shown.size()), static_cast<double>(sel), static_cast<double>(code)});
    d.pts.push_back({static_cast<double>(loads), 0.0, 0.0});
    pui::devPointsSet("plasma.browser", d);
  }
}

}  // namespace gl::packs::plasma::ui

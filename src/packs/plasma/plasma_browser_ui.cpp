// Gridline — "plasma" pack: the preset browser (covers the page and the lower
// panel). Sources: the factory presets, the user's Gridline preset library
// for Plasma, and the Serum library: the user's Serum 2 / Serum preset folders
// (indexed in the background, plasma_library_dsp.cpp, shown as a folder tree
// at any depth), opened through the same path as any preset file.
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

constexpr float kSideW = 230, kInfoX = 900, kTopY = kPageY, kRowH = 21;

std::string lowerExt(const std::string& file) {
  std::string s = fs::u8path(file).extension().u8string();
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

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
      if (it == rootLabel.end()) it = rootLabel.emplace(p.root, fs::u8path(p.root).filename().u8string()).first;
      folder = it->second;
    }
    out.add(p.name, folder, lowerExt(p.file) == ".fxp" ? "Serum" : "Serum 2", {3, -1, p.file});
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
  static bool dropAll = false, dropUser = false;  // REFRESH / SAVE AS: lists read again next frame
  if (dropAll || dropUser) {
    if (dropAll) cache.clear();
    else cache.erase("user");
    dropAll = dropUser = false;
    shownDirty = true;
  }
  const std::string libCache = libraryCache(c.a);
  libraryRefresh(libCache);
  const auto snap = librarySnapshot();
  if (snap->generation != libGen) {
    libGen = snap->generation;
    tree = libraryFolderTree(*snap);
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

  panelRect(c, 0, kTopY, kW, kStripY - kTopY, c.P.frame);
  // three frames: folders | list | preset info
  frameBox(c, kSideW, kTopY, kInfoX - 4 - kSideW, kStripY - kTopY);
  frameBox(c, kInfoX - 4, kTopY, kW - kInfoX + 4, kStripY - kTopY, c.P.panelLo);
  // ---- sidebar
  frameBox(c, 0, kTopY, kSideW, kStripY - kTopY, c.P.panelLo);
  pui::DevPoints devFolders;  // dev / tests: one point per folder row (k: -2 factory, -1 user, -3 all, else tree index)
  devFolders.x0 = c.p(0, 0).x, devFolders.x1 = c.p(kSideW, 0).x, devFolders.y0 = c.p(0, kTopY).y, devFolders.y1 = c.p(0, kStripY).y;
  const auto devRow = [&](float y, double code) {
    const double fx = 0.5, fv = (static_cast<double>(devFolders.y1) - c.p(0, y + kRowH * 0.5f).y) / std::max(1.0f, devFolders.y1 - devFolders.y0);
    devFolders.pts.push_back({fx, fv, code});
  };
  const int selCode = u.browserDir == "factory" ? -2 : u.browserDir == "user" ? -1 : u.browserDir == kSerum ? -3 : -4;
  float y = kTopY + 10;
  // a folder row: highlight, chevron (when it has sub-folders), label, count; returns 1 on a
  // click on the row, 2 on a click on the chevron
  auto row = [&](const char* id, const std::string& label, const std::string& key, int depth, int count, int kids, bool open) {
    int hit = 0;
    const bool on = u.browserDir == key;
    const float ax = 12 + depth * 12.0f;  // chevron centre
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(c.p(6, y));
    if (ImGui::InvisibleButton("##f", c.F.sz(kSideW - 12, kRowH))) {
      const float mx = ImGui::GetIO().MousePos.x;
      hit = kids && mx < c.p(ax + 7, 0).x ? 2 : 1;
    }
    const bool hv = ImGui::IsItemHovered();
    if (hv && isSerumScope(key) && key.size() > 6) c.a.setHint(key.find('\n') == std::string::npos ? key.substr(6) : key.substr(key.find('\n') + 1));
    if (on || hv) panelRect(c, 6, y, kSideW - 12, kRowH - 1, on ? c.P.accentSoft : c.P.panelHi, 3);
    if (on) c.dl->AddRectFilled(c.p(6, y + 3), c.p(8.5f, y + kRowH - 4), c.P.accent, c.s(1));
    if (kids) chevron(c, ax, y + kRowH * 0.5f - 0.5f, open, on || hv ? c.P.text : c.P.textFaint);
    const float lx = ax + 8;
    char cnt[24] = "";
    if (count >= 0) std::snprintf(cnt, sizeof cnt, "%d", count);
    const float cw = cnt[0] ? textW(c, cnt, ty::kCap) + 6 : 0;
    // the label is clipped before the count
    const ImVec2 c0 = c.p(lx, y), c1 = c.p(kSideW - 12 - cw, y + kRowH);
    c.dl->PushClipRect(c0, c1, true);
    text(c, lx, y + 5, label.c_str(), on ? c.P.text : (depth ? c.P.textDim : c.P.text), depth ? 8.5f : 9, depth == 0);
    c.dl->PopClipRect();
    if (cnt[0]) textR(c, kSideW - 12, y + 6.5f, cnt, c.P.textDim, ty::kCap);
    ImGui::PopID();
    return hit;
  };
  caption(c, 10, kSideW - 10, y, "PLASMA");
  y += 18;
  devRow(y, -2);
  if (row("fac", "Factory", "factory", 0, -1, 0, false)) u.browserDir = "factory";
  y += kRowH;
  devRow(y, -1);
  if (row("usr", "User presets", "user", 0, -1, 0, false)) u.browserDir = "user";
  y += kRowH + 8;
  caption(c, 10, kSideW - 10, y, snap->indexing ? "SERUM LIBRARY (INDEXING\xE2\x80\xA6)" : "SERUM LIBRARY");
  y += 18;
  static char addPath[512] = "";
  auto bottomButtons = [&](float by) {
    const float bw = (kSideW - 36) * 0.5f;
    if (button(c, "open", 16, by, bw, 20, "OPEN A FILE\xE2\x80\xA6", false, 8)) openPresetDialog(c);
    if (ImGui::IsItemHovered()) c.a.setHint("Open a Plasma preset, a Serum 2 preset (.SerumPreset) or a Serum preset (.fxp)");
    if (button(c, "addf", 20 + bw, by, bw, 20, "ADD FOLDER\xE2\x80\xA6", false, 8)) addPath[0] = 0, ImGui::OpenPopup("##addfolder");
    if (ImGui::IsItemHovered()) c.a.setHint("Add a folder of Serum presets to the library (its wavetables and samples are found there too)");
    if (ImGui::BeginPopup("##addfolder")) {
      ImGui::TextUnformatted(i18n::tr("Folder with Serum presets"));
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
    text(c, 16, y + 2, snap->indexing ? "Looking for Serum presets\xE2\x80\xA6" : "No Serum preset folders found", c.P.textDim, 8, true);
    y += 18;
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
    const float top = y, bottom = kStripY - 34, viewH = bottom - top;
    const float total = static_cast<float>(vis.size() + 1) * kRowH;
    const ImVec2 a0 = c.p(0, top), a1 = c.p(kSideW, bottom);
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
      if (row("sall", "All Serum presets", kSerum, 0, static_cast<int>(snap->presets.size()), 0, false)) u.browserDir = kSerum;
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
      ImGui::SetCursorScreenPos(c.p(kSideW - 6, top));
      ImGui::InvisibleButton("##fthumb", c.F.sz(6, viewH));
      const bool act = ImGui::IsItemActive(), hv = ImGui::IsItemHovered();
      if (act && ImGui::GetIO().MouseDelta.y != 0) treeScroll += ImGui::GetIO().MouseDelta.y / c.s(1) * (total - viewH) / std::max(1.f, viewH - th);
      if (ImGui::IsItemClicked()) {  // a click off the thumb jumps there
        const float my = (ImGui::GetIO().MousePos.y - a0.y) / c.s(1);
        if (my < ty - top || my > ty - top + th) treeScroll = std::clamp((my - th * 0.5f) / std::max(1.f, viewH - th), 0.f, 1.f) * (total - viewH);
      }
      panelRect(c, kSideW - 5, ty, 3, th, act || hv ? c.P.lineHi : c.P.line, 1.5f);
    }
    bottomButtons(kStripY - 28);
  }
  pui::devPointsSet("plasma.folders", devFolders);

  // ---- top bar: search, refresh, save, count, close
  {
    const float x = kSideW + 10, ty = kTopY + 8;
    char fb[128];
    std::snprintf(fb, sizeof fb, "%s", u.browserFilter.c_str());
    const char* hint = isSerumScope(scope) ? (scope == kSerum ? i18n::tr("Search the Serum library") : i18n::tr("Search this folder")) : i18n::tr("Search");
    if (inputField(c, "bsearch", x, ty, 300, 20, fb, sizeof fb, hint)) u.browserFilter = fb;
    if (button(c, "refresh", x + 310, ty, 66, 20, "REFRESH", false, 8)) {
      dropAll = true;
      libraryRefresh(libCache, true);
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Read the preset folders again");
    static char saveName[96] = "";
    if (button(c, "save", x + 380, ty, 66, 20, "SAVE AS\xE2\x80\xA6", false, 8)) {
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
          dropUser = true;
          if (ok) {
            c.m.presetName = m.presetName;
            commit(c, "Name preset");
          }
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    char b[48];
    std::snprintf(b, sizeof b, "%d presets", static_cast<int>(shown.size()));
    textR(c, kInfoX - 10, ty + 5, b, c.P.textDim, 8.5f, true);
    if (button(c, "close", kW - 70, ty, 62, 20, "CLOSE", false, 8)) u.browser = 0;
  }

  // ---- list: column heads (click: sort up, again: down, again: list order), rows
  const float lx = kSideW + 10, ly = kTopY + 38, lw = kInfoX - 10 - lx, lh = kStripY - 8 - ly;
  panelRect(c, lx, ly, lw, 20, c.P.head, 3);
  {
    const float colX[3] = {lx + 10, lx + lw * 0.55f, lx + lw - 70};
    const float colW[3] = {lw * 0.55f - 10, lw * 0.45f - 70, 70};
    const char* names[3] = {"NAME", "FOLDER", "SOURCE"};
    for (int k = 0; k < 3; ++k) {
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(c.p(colX[k] - 6, ly));
      if (ImGui::InvisibleButton("##sort", c.F.sz(colW[k], 20))) {
        if (sortCol != k) sortCol = k, sortDesc = false;
        else if (!sortDesc) sortDesc = true;
        else sortCol = -1, sortDesc = false;
      }
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.a.setHint("Sort by this column (click again: reverse, a third time: list order)");
      text(c, colX[k], ly + 6, names[k], sortCol == k ? c.P.text : (hv ? c.P.textDim : c.P.label), 8, true);
      if (sortCol == k) iconArrow(c.dl, c.p(colX[k] + textW(c, names[k], 8, true) + 7, ly + 10), c.s(5), sortDesc ? 3 : 2, c.P.accent);
      ImGui::PopID();
    }
  }
  wellRect(c, lx, ly + 22, lw, lh - 22, 3);
  ImGui::SetCursorScreenPos(c.p(lx, ly + 22));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  bool loadNow = false;
  int moveTo = -1;
  pui::DevPoints devRows;  // dev / tests: the visible preset rows (k = row in the list)
  if (ImGui::BeginChild("##plist", c.F.sz(lw, lh - 22), false)) {
    if (scrollTop) ImGui::SetScrollY(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = c.s(20);
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
        if (on || hv) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), on ? c.P.accentSoft : c.P.panelHi);
        else if (r % 2) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), theme::withAlpha(c.P.panelHi, 0.25f));
        ImFont* f = c.a.fontRegular;
        const float fsz = font(c, 9);
        const float ty = p0.y + (rowH - fsz) * 0.5f;
        // each column clipped to its width
        const float cx[3] = {p0.x + c.s(10), p0.x + c.s(lw * 0.55f), p0.x + c.s(lw - 70)};
        const std::string* cols[3] = {&e.name, &e.folder, &e.source};
        const ImU32 colC[3] = {on ? c.P.text : c.P.textDim, c.P.textDim, all.entries[ei].kind == 3 ? c.P.modText : c.P.textDim};
        for (int k = 0; k < 3; ++k) {
          const float x1 = k < 2 ? cx[k + 1] - c.s(8) : p0.x + c.s(lw);
          dl->PushClipRect(ImVec2(cx[k], p0.y), ImVec2(x1, p0.y + rowH), true);
          dl->AddText(f, fsz, ImVec2(cx[k], ty), colC[k], cols[k]->c_str());
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
    textC(c, lx + lw * 0.5f, ly + 60,
          !query.empty()                               ? "No presets match the search"
          : u.browserDir == "user"                     ? "No saved presets yet: SAVE AS stores the current sound here"
          : isSerumScope(scope) && snap->indexing      ? "Indexing the Serum library\xE2\x80\xA6"
                                                       : "No presets here",
          c.P.textDim, 9, true);
  if (loadNow && sel >= 0 && sel < static_cast<int>(shown.size())) {
    const size_t ei = static_cast<size_t>(shown[static_cast<size_t>(sel)]);
    if (load(c, all.entries[ei], all.rows[ei].name, infoText)) ++loads;
  }

  // ---- preset info
  {
    const float x = kInfoX, w = kW - kInfoX - 10, iy = kTopY + 34;
    caption(c, x + 8, x + w - 8, iy + 8, "PRESET");
    if (sel >= 0 && sel < static_cast<int>(shown.size())) {
      const size_t ei = static_cast<size_t>(shown[static_cast<size_t>(sel)]);
      const BrowserRow& e = all.rows[ei];
      c.dl->PushClipRect(c.p(x, iy), c.p(x + w, iy + 64), true);
      text(c, x + 10, iy + 28, e.name.c_str(), c.P.text, 11, true);
      text(c, x + 10, iy + 46, (e.folder + "  \xC2\xB7  " + e.source).c_str(), c.P.textDim, 8.5f, true);
      c.dl->PopClipRect();
      if (button(c, "load", x + 10, iy + 64, 80, 22, "OPEN", true, 8.5f) && load(c, all.entries[ei], e.name, infoText)) ++loads;
      if (ImGui::IsItemHovered()) c.a.setHint(all.entries[ei].file.empty() ? std::string("Open this preset") : all.entries[ei].file);
    } else {
      text(c, x + 10, iy + 30, "Select a preset", c.P.textDim, 9, true);
    }
    if (!infoText.empty()) {
      ImGui::SetCursorScreenPos(c.p(x + 10, iy + 96));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
      if (ImGui::BeginChild("##pinfo", c.F.sz(w - 20, kStripY - 16 - iy - 96), false)) {
        ImGui::PushStyleColor(ImGuiCol_Text, c.P.textDim);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(infoText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    textC(c, x + w * 0.5f, kStripY - 22, "Enter / double-click: open \xC2\xB7 Up / Down: audition", c.P.textDim, ty::kCap, false);
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

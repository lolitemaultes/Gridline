// Gridline — "plasma" pack: the preset browser (covers the page and the lower
// panel). Sources: the factory presets, the user's Gridline preset library
// for Plasma, and the Serum library: the user's Serum 2 / Serum preset folders
// (indexed in the background, plasma_library_dsp.cpp), opened through the same
// path as any preset file. Click: select, double-click / Enter: open, Up /
// Down: step and open.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include "../../core/I18n.h"
#include "../../core/Presets.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace fs = std::filesystem;

namespace {

struct Entry {
  std::string name, folder;
  int kind = 0;  // 0 factory (parameters), 1 factory (data), 2 user library, 3 Serum file
  int index = -1;
  std::string file;
};

constexpr float kSideW = 230, kInfoX = 900, kTopY = kPageY;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
std::string lowerExt(const std::string& file) { return lower(fs::u8path(file).extension().u8string()); }

std::vector<Entry> scan(const App& a, const std::string& src) {
  std::vector<Entry> out;
  if (src == "factory") {
    if (const PluginInfo* in = findPlugin(kType))
      for (int i = 0; i < static_cast<int>(in->presets.size()); ++i) out.push_back({in->presets[static_cast<size_t>(i)].name, "Factory", 0, i, ""});
    const auto& dp = factoryDataPresets();
    for (int i = 0; i < static_cast<int>(dp.size()); ++i) out.push_back({dp[static_cast<size_t>(i)].name, "Factory", 1, i, ""});
    return out;
  }
  if (src == "user") {
    for (const auto& e : listPresets(presetRoot(a.configDir()), kType)) out.push_back({e.name, e.folder.empty() ? "User" : e.folder, 2, -1, e.file});
    return out;
  }
  return out;
}

std::string libraryCache(const App& a) { return (fs::u8path(a.configDir()) / "plasma-library.json").u8string(); }

// Serum library scopes: "serum:" (all), "serum:<root>", "serum:<root>\n<bank>" (and below).
constexpr const char* kSerum = "serum:";
bool isSerumScope(const std::string& key) { return key.compare(0, 6, kSerum) == 0; }

std::vector<Entry> libraryEntries(const LibrarySnapshot& snap, const std::string& key) {
  std::vector<Entry> out;
  std::string root = key.substr(6), bank;
  if (const size_t nl = root.find('\n'); nl != std::string::npos) bank = root.substr(nl + 1), root = root.substr(0, nl);
  for (const auto& p : snap.presets) {
    if (!root.empty() && p.root != root) continue;
    if (!bank.empty() && p.bank != bank && p.bank.compare(0, bank.size() + 1, bank + "/") != 0) continue;
    const std::string label = fs::u8path(p.root).filename().u8string();
    out.push_back({p.name, p.bank.empty() ? label : p.bank, 3, -1, p.file});
  }
  return out;
}

// The library's folders: each root, its banks and their sub-folders.
struct Folder {
  std::string label, key;
  int depth = 0;
};
std::vector<Folder> libraryFolders(const LibrarySnapshot& snap) {
  std::vector<Folder> f;
  std::string lastRoot;
  std::set<std::string> banks;
  for (const auto& p : snap.presets) {
    if (p.root != lastRoot) {
      lastRoot = p.root;
      const std::string label = fs::u8path(p.root).filename().u8string();
      f.push_back({label.empty() ? p.root : label, kSerum + p.root, 0});
      banks.clear();
    }
    if (p.bank.empty()) continue;
    // the bank and its first sub-folder level
    const size_t s1 = p.bank.find('/');
    const std::string top = p.bank.substr(0, s1);
    if (banks.insert(top).second) f.push_back({top, kSerum + p.root + "\n" + top, 1});
    if (s1 != std::string::npos) {
      const size_t s2 = p.bank.find('/', s1 + 1);
      const std::string sub = p.bank.substr(0, s2);
      if (banks.insert(sub).second) f.push_back({sub.substr(s1 + 1), kSerum + p.root + "\n" + sub, 2});
    }
  }
  return f;
}

std::vector<std::pair<int, float>> valuesOf(const PluginState& st) {
  std::vector<std::pair<int, float>> v;
  for (size_t i = 0; i < st.params.size(); ++i) v.emplace_back(static_cast<int>(i), static_cast<float>(st.params[i]));
  return v;
}

void load(Ctx& c, const Entry& e, std::string& msg) {
  msg.clear();
  if (e.kind == 0) {
    if (const PluginInfo* in = findPlugin(kType)) applyPreset(c, in->presets[static_cast<size_t>(e.index)].values, "", e.name);
    msg = "Factory preset";
  } else if (e.kind == 1) {
    const auto& p = factoryDataPresets()[static_cast<size_t>(e.index)];
    applyPreset(c, p.values, p.data, p.name);
    msg = "Factory preset";
  } else if (e.kind == 2) {
    PluginState st;
    if (loadPreset(e.file, kType, st)) {
      applyPreset(c, valuesOf(st), st.data, e.name);
      msg = "Preset library: " + e.file;
    } else {
      msg = "Could not read " + e.file;
    }
  } else {  // Serum presets: the same Open path as the file dialog and drops
    if (openPreset(c, e.file)) {
      Model m = defaultModel();
      if (const PluginState* st = c.t.state(c.a.doc.song())) modelFromData(st->data, m);
      msg = openReportText(m);
    } else {
      msg = "Could not open " + e.file;
    }
  }
}

}  // namespace

void presetBrowser(Ctx& c) {
  Ui& u = c.u;
  if (u.browserDir.empty()) u.browserDir = "factory";
  static std::map<std::string, std::vector<Entry>> cache;
  static std::vector<Folder> folders;
  static uint64_t libGen = ~uint64_t(0);
  static int sel = -1;
  static std::string infoText;
  static std::string lastDir;
  static std::string lastScope;  // Serum scope of the list (searches cover the whole library)
  // the library index: listed at once from the cache, rescanned in the background
  const std::string libCache = libraryCache(c.a);
  libraryRefresh(libCache);
  const auto snap = librarySnapshot();
  if (snap->generation != libGen) {
    libGen = snap->generation;
    folders = libraryFolders(*snap);
    for (auto it = cache.begin(); it != cache.end();) it = isSerumScope(it->first) ? cache.erase(it) : std::next(it);
  }
  const std::string scope = isSerumScope(u.browserDir) && !u.browserFilter.empty() ? std::string(kSerum) : u.browserDir;
  if (!cache.count(scope)) cache[scope] = isSerumScope(scope) ? libraryEntries(*snap, scope) : scan(c.a, scope);
  if (lastDir != u.browserDir) sel = -1, infoText.clear(), lastDir = u.browserDir;
  if (lastScope != scope) sel = -1, lastScope = scope;
  const std::vector<Entry>& all = cache[scope];

  panelRect(c, 0, kTopY, kW, kStripY - kTopY, c.P.frame);
  // three frames: folders | list | preset info
  frameBox(c, kSideW, kTopY, kInfoX - 4 - kSideW, kStripY - kTopY);
  frameBox(c, kInfoX - 4, kTopY, kW - kInfoX + 4, kStripY - kTopY, c.P.panelLo);
  // ---- sidebar
  frameBox(c, 0, kTopY, kSideW, kStripY - kTopY, c.P.panelLo);
  float y = kTopY + 10;
  auto item = [&](const char* id, const std::string& label, const std::string& key, int depth) {
    const bool on = u.browserDir == key;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(c.p(6, y));
    if (ImGui::InvisibleButton("##f", c.F.sz(kSideW - 12, 20))) u.browserDir = key;
    const bool hv = ImGui::IsItemHovered();
    if (on || hv) panelRect(c, 6, y, kSideW - 12, 20, on ? c.P.accentSoft : c.P.panelHi, 3);
    if (on) c.dl->AddRectFilled(c.p(6, y + 3), c.p(8.5f, y + 17), c.P.accent, c.s(1));
    text(c, 16 + depth * 12, y + 5, label.c_str(), on ? c.P.text : (depth ? c.P.textDim : c.P.text), depth ? 8.5f : 9, depth == 0);
    ImGui::PopID();
    y += 21;
  };
  caption(c, 10, kSideW - 10, y, "PLASMA");
  y += 18;
  item("fac", "Factory", "factory", 0);
  item("usr", "User presets", "user", 0);
  y += 8;
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
  if (folders.empty()) {
    text(c, 16, y + 2, snap->indexing ? "Looking for Serum presets\xE2\x80\xA6" : "No Serum preset folders found", c.P.textFaint, 8, true);
    y += 18;
    bottomButtons(y);
  } else {
    // the folder list scrolls with the wheel when it is long
    static float scroll = 0;
    const float top = y, bottom = kStripY - 34;
    ImGui::SetCursorScreenPos(c.p(0, top));
    ImGui::InvisibleButton("##fsc", c.F.sz(kSideW, bottom - top));
    const bool hvList = ImGui::IsItemHovered();
    const float total = static_cast<float>(folders.size() + 1) * 21;
    if (hvList && ImGui::GetIO().MouseWheel != 0) scroll -= ImGui::GetIO().MouseWheel * 42;
    scroll = std::clamp(scroll, 0.f, std::max(0.f, total - (bottom - top)));
    c.dl->PushClipRect(c.p(0, top), c.p(kSideW, bottom), true);
    y = top - scroll;
    if (y + 21 >= top && y <= bottom) item("sall", "All Serum presets", kSerum, 0);
    else y += 21;
    for (size_t i = 0; i < folders.size(); ++i) {
      if (y + 21 < top || y > bottom) {
        y += 21;
        continue;
      }
      item(("sf" + std::to_string(i)).c_str(), folders[i].label, folders[i].key, folders[i].depth);
    }
    c.dl->PopClipRect();
    bottomButtons(kStripY - 28);
  }

  // ---- top bar: search, count, save, refresh, close
  {
    const float x = kSideW + 10, ty = kTopY + 8;
    char fb[128];
    std::snprintf(fb, sizeof fb, "%s", u.browserFilter.c_str());
    if (inputField(c, "bsearch", x, ty, 300, 20, fb, sizeof fb, i18n::tr("Search"))) u.browserFilter = fb, sel = -1;
    if (button(c, "refresh", x + 310, ty, 66, 20, "REFRESH", false, 8)) {
      cache.clear();
      libraryRefresh(libCache, true);
      return;
    }
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
          cache.erase("user");
          if (ok) {
            c.m.presetName = m.presetName;
            commit(c, "Name preset");
          }
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    if (button(c, "close", kW - 70, ty, 62, 20, "CLOSE", false, 8)) {
      u.browser = 0;
      return;
    }
  }

  // ---- list
  const std::string q = lower(u.browserFilter);
  std::vector<int> shown;
  for (int i = 0; i < static_cast<int>(all.size()); ++i) {
    const auto& e = all[static_cast<size_t>(i)];
    if (q.empty() || lower(e.name).find(q) != std::string::npos || lower(e.folder).find(q) != std::string::npos) shown.push_back(i);
  }
  {
    char b[48];
    std::snprintf(b, sizeof b, "%d presets", static_cast<int>(shown.size()));
    textR(c, kInfoX - 10, kTopY + 13, b, c.P.textDim, 8.5f, true);
  }
  const float lx = kSideW + 10, ly = kTopY + 38, lw = kInfoX - 10 - lx, lh = kStripY - 8 - ly;
  panelRect(c, lx, ly, lw, 20, c.P.head, 3);
  text(c, lx + 10, ly + 6, "NAME", c.P.label, 8, true);
  text(c, lx + lw * 0.55f, ly + 6, "FOLDER", c.P.label, 8, true);
  text(c, lx + lw - 70, ly + 6, "SOURCE", c.P.label, 8, true);
  wellRect(c, lx, ly + 22, lw, lh - 22, 3);
  ImGui::SetCursorScreenPos(c.p(lx, ly + 22));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  bool loadNow = false;
  int moveTo = -1;
  if (ImGui::BeginChild("##plist", c.F.sz(lw, lh - 22), false)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = c.s(20);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(shown.size()), rowH);
    while (clip.Step()) {
      for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
        const auto& e = all[static_cast<size_t>(shown[static_cast<size_t>(r)])];
        ImGui::PushID(r);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##row", ImVec2(c.s(lw), rowH))) sel = r;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) sel = r, loadNow = true;
        const bool on = sel == r, hv = ImGui::IsItemHovered();
        if (on || hv) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), on ? c.P.accentSoft : c.P.panelHi);
        else if (r % 2) dl->AddRectFilled(p0, ImVec2(p0.x + c.s(lw), p0.y + rowH), theme::withAlpha(c.P.panelHi, 0.25f));
        ImFont* f = c.a.fontRegular;
        const float fsz = font(c, 9);
        const float ty = p0.y + (rowH - fsz) * 0.5f;
        dl->AddText(f, fsz, ImVec2(p0.x + c.s(10), ty), on ? c.P.text : c.P.textDim, e.name.c_str());
        dl->AddText(f, fsz, ImVec2(p0.x + c.s(lw * 0.55f), ty), c.P.textFaint, e.folder.c_str());
        const char* src = e.kind <= 1 ? "Factory" : e.kind == 2 ? "Library" : lowerExt(e.file) == ".fxp" ? "Serum" : "Serum 2";
        dl->AddText(f, fsz, ImVec2(p0.x + c.s(lw - 70), ty), e.kind == 3 ? c.P.mod : c.P.textFaint, src);
        ImGui::PopID();
      }
    }
    if (ImGui::IsWindowFocused() && !shown.empty()) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) moveTo = std::min(static_cast<int>(shown.size()) - 1, sel + 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) moveTo = std::max(0, sel - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) && sel >= 0) loadNow = true;
    }
    if (moveTo >= 0) {
      sel = moveTo, loadNow = true;
      const float yr = static_cast<float>(sel) * rowH;
      if (yr < ImGui::GetScrollY()) ImGui::SetScrollY(yr);
      if (yr + rowH > ImGui::GetScrollY() + ImGui::GetWindowHeight()) ImGui::SetScrollY(yr + rowH - ImGui::GetWindowHeight());
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (shown.empty())
    textC(c, lx + lw * 0.5f, ly + 60,
          u.browserDir == "user" ? "No saved presets yet: SAVE AS stores the current sound here"
          : isSerumScope(scope) && snap->indexing ? "Indexing the Serum library\xE2\x80\xA6"
                                                    : "No presets here",
          c.P.textFaint, 9, true);
  if (loadNow && sel >= 0 && sel < static_cast<int>(shown.size())) load(c, all[static_cast<size_t>(shown[static_cast<size_t>(sel)])], infoText);

  // ---- infoText pane
  {
    const float x = kInfoX, w = kW - kInfoX - 10, iy = kTopY + 34;
    caption(c, x + 8, x + w - 8, iy + 8, "PRESET");
    if (sel >= 0 && sel < static_cast<int>(shown.size())) {
      const auto& e = all[static_cast<size_t>(shown[static_cast<size_t>(sel)])];
      text(c, x + 10, iy + 28, e.name.c_str(), c.P.text, 11, true);
      text(c, x + 10, iy + 46, e.folder.c_str(), c.P.textDim, 8.5f, true);
      if (button(c, "load", x + 10, iy + 64, 80, 22, "OPEN", true, 8.5f)) load(c, e, infoText);
      if (ImGui::IsItemHovered()) c.a.setHint("Open this preset");
    } else {
      text(c, x + 10, iy + 30, "Select a preset", c.P.textFaint, 9, true);
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
    char hint[64];
    std::snprintf(hint, sizeof hint, "%s", "Enter / double-click: open \xC2\xB7 Up / Down: audition");
    textC(c, x + w * 0.5f, kStripY - 22, hint, c.P.textFaint, 7.5f, true);
  }
}

}  // namespace gl::packs::plasma::ui

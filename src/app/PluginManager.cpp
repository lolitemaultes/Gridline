// Gridline: Options > Plugin manager (J-147 redesign). One row per plugin
// with the formats it is installed in (host/PluginGroups.h), the preferred
// format the browser lists, favourites, clean categories and failed plugins
// with retry. Left: scan status, formats and search folders. Right: view
// tabs with counts, search, the sortable plugin table and the details of the
// selected plugin (a side panel, or below the table in a narrow window).
// Gridline's own Suite builds sit in a collapsed group: the native plugins
// are built in, so the browser never lists them.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginGroups.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "BrowserUi.h"
#include "HostedUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace fs = std::filesystem;

namespace gl {

using theme::pal;

namespace {

// Category names as host::normaliseCategory returns them (English), marked
// for the translation template; drawn through i18n::tr.
[[maybe_unused]] const char* const kCategoryText[] = {
    i18n::N_("Drums"),    i18n::N_("Keys"),        i18n::N_("Sampler"),  i18n::N_("Synth"),    i18n::N_("Instrument"),
    i18n::N_("Analyzer"), i18n::N_("Restoration"), i18n::N_("Reverb"),   i18n::N_("Delay"),    i18n::N_("EQ"),
    i18n::N_("Filter"),   i18n::N_("Dynamics"),    i18n::N_("Distortion"), i18n::N_("Modulation"), i18n::N_("Pitch"),
    i18n::N_("Spatial"),  i18n::N_("Spectral"),    i18n::N_("MIDI"),     i18n::N_("Mastering"), i18n::N_("Utility"),
    i18n::N_("Effect")};

enum View { kAll, kInstruments, kEffects, kFavourites, kFailed, kViewCount };

struct ManagerState {
  char search[96] = "";
  char folder[512] = "";
  char category[64] = "";
  int view = kAll;
  std::string selected;     // group key of the selected plugin
  std::string selectedFail;  // "<format>\n<entry>" of the selected failed entry
  std::string categoryFor;   // group key the category field was filled for
  bool suiteOpen = false;    // the Gridline Suite group is expanded
  bool focusSearch = false;
  bool scrollToSelection = false;
  int sortColumn = 1;  // table column the rows are sorted by
  bool sortAscending = true;
};
ManagerState& state() {
  static ManagerState s;
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string groupKey(const host::PluginGroup& g) { return host::groupNameKey(g.name) + "|" + host::groupVendorKey(g.vendor) + (g.ownSuite ? "|suite" : ""); }

// "~/Library/..." for paths below the home folder.
std::string tildePath(const std::string& p) {
  const std::string home = platform::homeDir();
  if (home.size() > 1 && p.size() > home.size() && p.compare(0, home.size(), home) == 0 && (p[home.size()] == '/' || p[home.size()] == '\\'))
    return "~" + p.substr(home.size());
  return p;
}

const char* formatLabel(const std::string& id) {
  const host::Format* f = host::findFormat(id);
  return f ? f->label : id.c_str();
}

// ---------------------------------------------------------------- colours

struct Colors {
  ImU32 panel, panelLine, rowHover, rowSel, selBar, badgeLine, chipOn, chipOnText, field;
};
Colors colors() {
  const auto& P = pal();
  Colors c;
  c.panel = P.dark ? theme::mix(P.surface900, P.surface800, 0.30f) : theme::mix(P.surface800, P.paper, 0.55f);
  c.panelLine = P.dark ? theme::mix(P.line, P.surface800, 0.35f) : P.line;
  c.rowHover = theme::withAlpha(P.text, P.dark ? 0.05f : 0.06f);
  c.rowSel = theme::withAlpha(P.accent, P.dark ? 0.20f : 0.14f);
  c.selBar = P.accent;
  c.badgeLine = theme::withAlpha(P.textFaint, 0.55f);
  c.chipOn = theme::mix(P.dark ? P.surface700 : P.surface800, P.hot, P.dark ? 0.16f : 0.12f);
  c.chipOnText = theme::lit(P.hot);
  c.field = P.field;
  return c;
}

// Text that fits `maxW` at a font / size, the ellipsis in the middle (paths).
std::string fitMiddle(ImFont* font, float size, const std::string& s, float maxW) {
  if (font->CalcTextSizeA(size, 1e9f, 0, s.c_str()).x <= maxW) return s;
  const std::string dots = "\xE2\x80\xA6";
  size_t keep = s.size();
  while (keep > 2) {
    --keep;
    const size_t head = keep / 3, tail = keep - head;
    std::string t = s.substr(0, head) + dots + s.substr(s.size() - tail);
    if (font->CalcTextSizeA(size, 1e9f, 0, t.c_str()).x <= maxW) return t;
  }
  return dots;
}

void text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* s, float maxW = 0) {
  if (maxW > 0) theme::textEllipsis(dl, font, size, ImVec2(std::round(pos.x), std::round(pos.y)), maxW, col, s);
  else dl->AddText(font, size, ImVec2(std::round(pos.x), std::round(pos.y)), col, s);
}
float textW(ImFont* font, float size, const char* s) { return font->CalcTextSizeA(size, 1e9f, 0, s).x; }

// Format tag: a small pill. `preferred`: filled with the lit colour.
float badge(ImDrawList* dl, ImFont* mono, ImVec2 pos, float h, const char* label, bool preferred, bool faint = false, float minW = 0) {
  const auto& P = pal();
  const Colors C = colors();
  const float fs = 10.0f;
  const float w = std::max(minW, std::round(textW(mono, fs, label) + 10));
  const ImVec2 a(std::round(pos.x), std::round(pos.y)), b(a.x + w, a.y + h);
  if (preferred) {
    dl->AddRectFilled(a, b, theme::withAlpha(P.hot, P.dark ? 0.20f : 0.16f), theme::kRadius);
    dl->AddRect(a, b, theme::withAlpha(P.hot, 0.55f), theme::kRadius);
  } else {
    dl->AddRect(a, b, faint ? theme::withAlpha(C.badgeLine, 0.5f) : C.badgeLine, theme::kRadius);
  }
  const ImU32 tc = preferred ? theme::lit(P.hot) : faint ? P.textFaint : P.textDim;
  text(dl, mono, fs, ImVec2(a.x + (w - textW(mono, fs, label)) * 0.5f, a.y + (h - fs) * 0.5f - 0.5f), tc, label);
  return w;
}

// Section caption with an optional right-aligned note.
void caption(App& a, const char* label, const char* note = nullptr) {
  const auto& P = pal();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const char* t = i18n::tr(label);
  text(dl, a.fontBold, theme::kFontLabel, p, P.textDim, t);
  if (note) {
    const float nw = textW(a.fontMono, 10.5f, note);
    text(dl, a.fontMono, 10.5f, ImVec2(p.x + w - nw, p.y + 0.5f), P.textFaint, note);
  }
  ImGui::Dummy(ImVec2(w, theme::kFontLabel + theme::kS2));
}

// Translated "%d" text.
std::string fmtCount(const char* english, int n) {
  char b[160];
  std::snprintf(b, sizeof b, i18n::tr(english), n);
  return b;
}

// ---------------------------------------------------------------- view rows

struct Row {
  const host::PluginGroup* g = nullptr;  // nullptr: the Gridline Suite group header
  std::string key, category;             // group key, shown category
  bool fav = false;
  int sandbox = 0;  // members in the sandbox: 0 none, 1 some, 2 all
};

struct Model {
  std::shared_ptr<const host::GroupSnapshot> snap;
  std::vector<Row> rows;       // visible rows (suite header and its rows at the end)
  int counts[kViewCount] = {};  // per view (Suite builds not counted)
  int suiteCount = 0;
  std::vector<host::CacheEntry> failed;
};

std::string shownCategory(const host::GroupSnapshot& s, const host::PluginGroup& g, const host::PluginPrefs& prefs) {
  for (size_t m : g.members)
    if (auto it = prefs.categories.find(s.items[m].type); it != prefs.categories.end()) return it->second;
  return g.category;
}

bool groupFav(const host::GroupSnapshot& s, const host::PluginGroup& g, const host::PluginPrefs& prefs) {
  for (size_t m : g.members)
    if (prefs.favourites.count(s.items[m].type)) return true;
  return false;
}

int groupSandbox(const host::GroupSnapshot& s, const host::PluginGroup& g, const host::PluginPrefs& prefs) {
  size_t n = 0;
  for (size_t m : g.members) n += prefs.sandbox.count(s.items[m].type);
  return n == 0 ? 0 : n == g.members.size() ? 2 : 1;
}

bool matches(const host::GroupSnapshot& s, const host::PluginGroup& g, const std::string& cat, const std::string& q) {
  if (q.empty()) return true;
  if (lower(g.name).find(q) != std::string::npos || lower(g.vendor).find(q) != std::string::npos || lower(cat).find(q) != std::string::npos ||
      lower(i18n::tr(cat.c_str())).find(q) != std::string::npos)
    return true;
  for (size_t m : g.members)
    if (lower(formatLabel(s.items[m].format)) == q) return true;
  return false;
}

Model buildModel(const host::PluginPrefs& prefs, const std::string& q) {
  ManagerState& ms = state();
  Model md;
  md.snap = host::pluginGroups();
  const host::GroupSnapshot& s = *md.snap;
  for (const auto& e : host::cacheEntries())
    if (!e.ok && !prefs.disabledFormats.count(e.format)) md.failed.push_back(e);
  md.counts[kFailed] = static_cast<int>(md.failed.size());
  std::vector<Row> main, suite;
  for (const host::PluginGroup& g : s.groups) {
    Row r;
    r.g = &g;
    r.category = shownCategory(s, g, prefs);
    r.fav = groupFav(s, g, prefs);
    r.sandbox = groupSandbox(s, g, prefs);
    if (g.ownSuite) {
      ++md.suiteCount;
    } else {
      ++md.counts[kAll];
      ++md.counts[g.instrument ? kInstruments : kEffects];
      if (r.fav) ++md.counts[kFavourites];
    }
    if (ms.view == kInstruments && !g.instrument) continue;
    if (ms.view == kEffects && g.instrument) continue;
    if (ms.view == kFavourites && (!r.fav || g.ownSuite)) continue;
    if (!matches(s, g, r.category, q)) continue;
    r.key = groupKey(g);
    (g.ownSuite ? suite : main).push_back(std::move(r));
  }
  auto cmp = [&](const Row& x, const Row& y) {
    std::string a1, b1;
    switch (ms.sortColumn) {
      case 3: a1 = lower(x.category), b1 = lower(y.category); break;
      case 4: a1 = lower(x.g->vendor), b1 = lower(y.g->vendor); break;
      default: break;
    }
    if (a1 == b1) a1 = lower(x.g->name), b1 = lower(y.g->name);
    if (a1 == b1) return false;
    return ms.sortAscending ? a1 < b1 : a1 > b1;
  };
  std::stable_sort(main.begin(), main.end(), cmp);
  std::stable_sort(suite.begin(), suite.end(), cmp);
  md.rows = std::move(main);
  if (!suite.empty()) {
    Row h;
    h.key = "##suite";
    md.rows.push_back(h);
    if (ms.suiteOpen)
      for (auto& r : suite) md.rows.push_back(std::move(r));
  }
  return md;
}

// ---------------------------------------------------------------- actions

void setFavourite(const host::GroupSnapshot& s, const host::PluginGroup& g, host::PluginPrefs& prefs, bool on) {
  for (size_t m : g.members) prefs.favourites.erase(s.items[m].type);
  if (on) prefs.favourites.insert(s.items[g.preferred].type);
}

void setSandbox(App& a, const host::GroupSnapshot& s, const host::PluginGroup& g, bool on) {
  for (size_t m : g.members) setHostedSandbox(a, s.items[m].type, on);  // saves the setting itself
}

// The browser lists `type` for this plugin from now on.
void setPreferred(const host::GroupSnapshot& s, const host::PluginGroup& g, host::PluginPrefs& prefs, size_t member) {
  const bool fav = groupFav(s, g, prefs);
  for (size_t m : g.members) prefs.preferTypes.erase(s.items[m].type);
  // Only a pick that differs from the global order is stored.
  const std::vector<std::string> order = host::formatOrder(prefs.preferredFormat);
  size_t auto_ = g.members.front();
  int best = 1 << 20;
  for (size_t m : g.members) {
    const int r = static_cast<int>(std::find(order.begin(), order.end(), s.items[m].format) - order.begin());
    if (r < best) best = r, auto_ = m;
  }
  if (member != auto_) prefs.preferTypes.insert(s.items[member].type);
  if (fav) {  // the favourite follows the listed format
    for (size_t m : g.members) prefs.favourites.erase(s.items[m].type);
    prefs.favourites.insert(s.items[member].type);
  }
}

void addToProject(App& a, const std::string& type, bool instrument) {
  if (instrument) {
    a.settingsChannel = a.addInstrument(type);
    return;
  }
  const Song& s = a.doc.song();
  ObjectId mid = a.selMixer != kNoId ? a.selMixer : (s.mixer.size() > 1 ? s.mixer[1].id : kNoId);
  if (const MixerTrack* t = s.mixerTrack(mid)) {
    for (int k = 0; k < kEffectSlots; ++k)
      if (t->slots[static_cast<size_t>(k)].plugin.empty()) {
        a.doc.setSlotPlugin(t->id, k, type);
        a.fxEditor = {t->id, k};
        a.selMixer = t->id;
        return;
      }
    a.notify("No free effect slot on the selected mixer track", true);
  }
}

void chooseFolder(App& a) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::N_("Add a plugin folder");
  a.fileDialog.dir = platform::homeDir();
  a.fileDialog.exts = {"/"};  // folder mode
  a.fileDialog.onOk = [](const std::string& dir) {
    host::PluginPrefs p = host::pluginPrefs();
    if (std::find(p.extraFolders.begin(), p.extraFolders.end(), dir) == p.extraFolders.end()) {
      p.extraFolders.push_back(dir);
      host::setPluginPrefs(p);
    }
  };
}

// ---------------------------------------------------------------- left column

void scanCard(App& a, const Model& md) {
  const auto& P = pal();
  ManagerState& ms = state();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const host::ScanStatus ss = host::scanStatus();
  const float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  char num[32];
  std::snprintf(num, sizeof num, "%d", md.counts[kAll]);
  text(dl, a.fontBold, 24.0f, ImVec2(p.x, p.y - 2), P.text, num);
  const float nw = textW(a.fontBold, 24.0f, num);
  const std::string unit = md.counts[kAll] == 1 ? std::string(i18n::tr("plugin")) : std::string(i18n::tr("plugins"));
  text(dl, a.fontRegular, theme::kFontBody, ImVec2(p.x + nw + 6, p.y + 7), P.textDim, unit.c_str());
  ImGui::Dummy(ImVec2(w, 28));
  // Status line: scanning progress, failed plugins, or all clear.
  p = ImGui::GetCursorScreenPos();
  if (ss.running) {
    const float frac = ss.total > 0 ? std::clamp(static_cast<float>(ss.done) / static_cast<float>(ss.total), 0.0f, 1.0f) : 0.0f;
    dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + w, p.y + 8), colors().field, 2.0f);
    dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + std::max(4.0f, w * frac), p.y + 8), P.accent, 2.0f);
    char b[96];
    std::snprintf(b, sizeof b, i18n::tr("Scanning %d / %d"), std::min(ss.done + 1, std::max(1, ss.total)), ss.total);
    text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x, p.y + 13), P.text, b);
    const std::string cur = fs::path(ss.current).filename().string();
    const float bw = textW(a.fontRegular, theme::kFontCaption, b) + 8;
    text(dl, a.fontMono, 10.5f, ImVec2(p.x + bw, p.y + 14), P.textFaint, cur.c_str(), w - bw);
    ImGui::Dummy(ImVec2(w, 32));
  } else {
    if (md.counts[kFailed] > 0) {
      const std::string t = fmtCount(md.counts[kFailed] == 1 ? i18n::N_("%d plugin failed to scan") : i18n::N_("%d plugins failed to scan"), md.counts[kFailed]);
      ImGui::SetNextItemAllowOverlap();
      const bool click = ImGui::InvisibleButton("##failedlink", ImVec2(w, 18));
      const bool hov = ImGui::IsItemHovered();
      ui::drawIcon(dl, ui::Icon::Info, ImVec2(p.x + 6, p.y + 9), 12, P.danger);
      text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + 17, p.y + 2), hov ? P.text : P.danger, t.c_str(), w - 17);
      if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ui::Tooltip("Show the plugins that crashed, hung or failed to load");
      }
      if (click) ms.view = kFailed;
    } else {
      ui::drawIcon(dl, ui::Icon::Check, ImVec2(p.x + 6, p.y + 9), 12, theme::lit(P.led));
      text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + 17, p.y + 2), P.textDim, i18n::tr("Every plugin scanned cleanly"), w - 17);
      ImGui::Dummy(ImVec2(w, 18));
    }
    ImGui::Dummy(ImVec2(0, 4));
  }
  const float bw = std::floor((w - 6) * 0.5f);
  ImGui::BeginDisabled(ss.running);
  if (ui::TextButton("Find plugins", true, bw, "Scans new and updated plugins in the folders below (each in its own process)")) {
    host::startScan(host::selfExecutablePath(), false);
    a.scanByUser = true;
  }
  ImGui::SameLine(0, 6);
  if (ui::TextButton("Rescan all", false, bw, "Scans every plugin again, including failed ones")) {
    host::startScan(host::selfExecutablePath(), true);
    a.scanByUser = true;
  }
  ImGui::EndDisabled();
}

void formatChips(App& a, const Model& md, host::PluginPrefs& prefs, bool& changed) {
  const auto& P = pal();
  const Colors C = colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  std::map<std::string, int> perFormat;
  for (const auto& e : host::cacheEntries())
    if (e.ok) perFormat[e.format] += std::max(0, e.classes);
  (void)md;
  const auto& fmts = host::formats();
  const float w = ImGui::GetContentRegionAvail().x, gap = 6, h = 26;
  const int cols = w >= 3 * 72 + 2 * gap ? 3 : 2;
  const float cw = std::floor((w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  for (size_t i = 0; i < fmts.size(); ++i) {
    const host::Format& f = fmts[i];
    const bool on = !prefs.disabledFormats.count(f.id);
    const ImVec2 q(origin.x + static_cast<float>(static_cast<int>(i) % cols) * (cw + gap), origin.y + static_cast<float>(static_cast<int>(i) / cols) * (h + gap));
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(f.id);
    if (ImGui::InvisibleButton("##chip", ImVec2(cw, h))) {
      if (on) prefs.disabledFormats.insert(f.id);
      else prefs.disabledFormats.erase(f.id);
      changed = true;
    }
    const bool hov = ImGui::IsItemHovered();
    ui::FocusRing();
    if (hov) ui::Tooltip(on ? "On: scanned and listed (click to switch off)" : "Off: not scanned, hidden everywhere (click to switch on)");
    const ImVec2 b(q.x + cw, q.y + h);
    if (on) {
      dl->AddRectFilled(q, b, hov ? theme::mix(C.chipOn, P.hot, 0.08f) : C.chipOn, theme::kRadiusLg);
      dl->AddRect(q, b, theme::withAlpha(P.hot, 0.45f), theme::kRadiusLg);
      ui::drawIcon(dl, ui::Icon::Check, ImVec2(q.x + 12, q.y + h * 0.5f), 11, C.chipOnText);
    } else {
      dl->AddRectFilled(q, b, hov ? C.rowHover : theme::withAlpha(C.field, 0.6f), theme::kRadiusLg);
      dl->AddRect(q, b, C.badgeLine, theme::kRadiusLg);
      dl->AddCircle(ImVec2(q.x + 12, q.y + h * 0.5f), 4.5f, P.textFaint, 0, 1.2f);
    }
    text(dl, a.fontBold, theme::kFontCaption, ImVec2(q.x + 22, q.y + (h - theme::kFontCaption) * 0.5f - 1), on ? C.chipOnText : P.textFaint, f.label);
    const auto it = perFormat.find(f.id);
    char n[16];
    std::snprintf(n, sizeof n, "%d", it == perFormat.end() ? 0 : it->second);
    const float nw = textW(a.fontMono, 10.5f, n);
    text(dl, a.fontMono, 10.5f, ImVec2(b.x - nw - 7, q.y + (h - 10.5f) * 0.5f - 0.5f), on ? P.textDim : P.textFaint, on ? n : i18n::tr("off"));
    ImGui::PopID();
  }
  const int rowsN = (static_cast<int>(fmts.size()) + cols - 1) / cols;
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(w, static_cast<float>(rowsN) * (h + gap) - gap));

  // Preferred format: what the browser lists when a plugin comes in several.
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Preferred format"));
  ui::Hint("The browser lists each plugin once, in this format when it is installed in several (each plugin can override it)");
  ImGui::SameLine();
  const float cbw = std::max(80.0f, ImGui::GetContentRegionAvail().x);
  std::string preview = prefs.preferredFormat.empty() ? std::string(i18n::tr("Automatic")) : std::string(formatLabel(prefs.preferredFormat));
  if (ui::BeginCombo("##prefFormat", preview.c_str(), cbw)) {
    if (ImGui::Selectable(i18n::tr("Automatic (VST3, CLAP, AU, LV2, VST2)"), prefs.preferredFormat.empty())) {
      prefs.preferredFormat.clear();
      changed = true;
    }
    for (const host::Format& f : fmts) {
      if (ImGui::Selectable(f.label, prefs.preferredFormat == f.id)) {
        prefs.preferredFormat = f.id;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  bool all = prefs.browserAllFormats;
  if (ImGui::Checkbox(i18n::tr("List every format in the browser"), &all)) {
    prefs.browserAllFormats = all;
    changed = true;
  }
  ui::Hint("Off: one entry per plugin in the browser and the plugin picker");
}

struct FolderRow {
  std::string tag, path;
  bool user = false, exists = true;
  size_t index = 0;  // user folder index
};

void folderList(App& a, host::PluginPrefs& prefs, bool& changed, float height) {
  const auto& P = pal();
  const Colors C = colors();
  std::vector<FolderRow> rows;
  for (const host::Format& f : host::formats()) {
    if (prefs.disabledFormats.count(f.id)) continue;
    for (const auto& p : f.defaultPaths()) {
      std::error_code ec;
      rows.push_back(FolderRow{f.label, p, false, fs::is_directory(p, ec), 0});
    }
  }
  for (size_t i = 0; i < prefs.extraFolders.size(); ++i) {
    std::error_code ec;
    rows.push_back(FolderRow{"", prefs.extraFolders[i], true, fs::is_directory(prefs.extraFolders[i], ec), i});
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(C.field));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusLg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::BeginChild("##folders", ImVec2(0, height), ImGuiChildFlags_None);
  ImGui::PopStyleVar(2);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rowH = 22, tagW = 38;
  int removeAt = -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    const FolderRow& r = rows[i];
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(w, rowH));
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), C.rowHover, theme::kRadius);
    if (r.user) badge(dl, a.fontMono, ImVec2(p.x + 2, p.y + 3), rowH - 6, i18n::tr("USER"), false, false, tagW);
    else badge(dl, a.fontMono, ImVec2(p.x + 2, p.y + 3), rowH - 6, r.tag.c_str(), false, !r.exists, tagW);
    const float x0 = p.x + tagW + 9;
    float right = p.x + w - 4;
    if (r.user) right -= 20;
    if (!r.exists) {
      const char* miss = i18n::tr("missing");
      const float mw = textW(a.fontRegular, theme::kFontLabel, miss);
      text(dl, a.fontRegular, theme::kFontLabel, ImVec2(right - mw, p.y + (rowH - theme::kFontLabel) * 0.5f - 1), r.user ? P.danger : P.textFaint, miss);
      right -= mw + 8;
    }
    const std::string shown = fitMiddle(a.fontMono, 10.5f, tildePath(r.path), right - x0);
    text(dl, a.fontMono, 10.5f, ImVec2(x0, p.y + (rowH - 10.5f) * 0.5f - 0.5f), r.exists ? P.text : P.textFaint, shown.c_str());
    if (hov && !ImGui::IsAnyItemActive()) {
      std::string tip = r.path + "\n";
      if (r.user) tip += i18n::tr("Your folder: searched for every format");
      else tip += std::string(i18n::tr("Default folder for ")) + r.tag;
      if (!r.exists) tip += std::string("\n") + i18n::tr("Not found on this computer: nothing to scan here");
      ImGui::SetTooltip("%s", tip.c_str());
    }
    if (r.user) {
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - 21, p.y + 2));
      if (ui::IconButton("##rm", ui::Icon::Trash, false, 18, "Remove this folder")) removeAt = static_cast<int>(r.index);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::PopID();
  }
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (removeAt >= 0) {
    prefs.extraFolders.erase(prefs.extraFolders.begin() + removeAt);
    changed = true;
  }
  // Add: a typed / pasted path, or the folder chooser.
  ManagerState& ms = state();
  const float bw = ui::TextButtonWidth("Add");
  ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - bw - 26 - 8));
  const bool enter = ImGui::InputTextWithHint("##addfolder", i18n::tr("Paste a folder path"), ms.folder, sizeof ms.folder, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine(0, 4);
  if (ui::IconButton("##pick", ui::Icon::Folder, false, ImGui::GetFrameHeight(), "Choose a folder to search for plugins")) chooseFolder(a);
  ImGui::SameLine(0, 4);
  if ((ui::TextButton("Add", false, bw, "Also search this folder for plugins of every format") || enter) && ms.folder[0]) {
    std::string f = ms.folder;
    if (f.size() > 1 && f[0] == '~' && (f[1] == '/' || f[1] == '\\')) f = platform::homeDir() + f.substr(1);
    std::error_code ec;
    if (!fs::is_directory(f, ec)) {
      a.notify("Not a folder: " + f, true);
    } else if (std::find(prefs.extraFolders.begin(), prefs.extraFolders.end(), f) == prefs.extraFolders.end()) {
      prefs.extraFolders.push_back(f);
      changed = true;
      ms.folder[0] = 0;
    }
  }
}

void leftColumn(App& a, const Model& md, host::PluginPrefs& prefs, bool& changed) {
  caption(a, i18n::N_("SCAN"));
  scanCard(a, md);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  caption(a, i18n::N_("FORMATS"));
  formatChips(a, md, prefs, changed);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  caption(a, i18n::N_("SEARCH FOLDERS"));
  const float addH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 2;
  folderList(a, prefs, changed, std::max(110.0f, ImGui::GetContentRegionAvail().y - addH - ImGui::GetStyle().ItemSpacing.y));
}

// ---------------------------------------------------------------- right column

// View tabs with counts (the segmented control's look).
void viewTabs(App& a, const Model& md, float maxW) {
  const auto& P = pal();
  ManagerState& ms = state();
  static const char* const labels[kViewCount] = {i18n::N_("All"), i18n::N_("Instruments"), i18n::N_("Effects"), i18n::N_("Favourites"), i18n::N_("Failed")};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetFrameHeight() + 4;
  const float pad = 10, gapN = 6;
  float ws[kViewCount], total = 4;
  char nums[kViewCount][16];
  for (int i = 0; i < kViewCount; ++i) {
    std::snprintf(nums[i], sizeof nums[i], "%d", md.counts[i]);
    ws[i] = textW(a.fontRegular, ImGui::GetFontSize(), i18n::tr(labels[i])) + gapN + textW(a.fontMono, 10.5f, nums[i]) + pad * 2;
    total += ws[i];
  }
  const bool compact = total > maxW;  // narrow: counts hidden
  if (compact) {
    total = 4;
    for (int i = 0; i < kViewCount; ++i) total += (ws[i] = textW(a.fontRegular, ImGui::GetFontSize(), i18n::tr(labels[i])) + 14);
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), P.field, theme::kRadiusLg);
  if (!P.dark) dl->AddRect(p, ImVec2(p.x + total, p.y + h), P.line, theme::kRadiusLg);
  float x = p.x + 2;
  for (int i = 0; i < kViewCount; ++i) {
    ImGui::PushID(i);
    const ImVec2 q(x, p.y + 2), q1(x + ws[i], p.y + h - 2);
    ImGui::SetCursorScreenPos(q);
    if (ImGui::InvisibleButton("##tab", ImVec2(ws[i], h - 4)) && ms.view != i) ms.view = i;
    const bool hov = ImGui::IsItemHovered(), on = ms.view == i;
    ui::FocusRing();
    if (on) dl->AddRectFilled(q, q1, theme::mix(P.dark ? P.surface700 : P.surface800, P.hot, 0.14f), theme::kRadius);
    else if (hov) dl->AddRectFilled(q, q1, P.dark ? P.surface700 : theme::withAlpha(P.text, 0.06f), theme::kRadius);
    const char* l = i18n::tr(labels[i]);
    const float fs = ImGui::GetFontSize();
    const ImU32 tc = on ? theme::lit(P.hot) : hov ? P.text : P.textDim;
    const bool danger = i == kFailed && md.counts[i] > 0;
    if (compact) {
      text(dl, a.fontRegular, fs, ImVec2(q.x + 7, q.y + (h - 4 - fs) * 0.5f), tc, l);
    } else {
      const float lw = textW(a.fontRegular, fs, l);
      text(dl, a.fontRegular, fs, ImVec2(q.x + pad, q.y + (h - 4 - fs) * 0.5f), tc, l);
      text(dl, a.fontMono, 10.5f, ImVec2(q.x + pad + lw + gapN, q.y + (h - 4 - 10.5f) * 0.5f), danger ? P.danger : on ? tc : P.textFaint, nums[i]);
    }
    x += ws[i];
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(total, h));
}

void searchBox(App& a, float w) {
  const auto& P = pal();
  ManagerState& ms = state();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetFrameHeight() + 4;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28, ImGui::GetStyle().FramePadding.y + 2));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kRadiusLg);
  ImGui::SetNextItemWidth(w);
  if (ms.focusSearch) {
    ImGui::SetKeyboardFocusHere();
    ms.focusSearch = false;
  }
  ImGui::InputTextWithHint("##search", w >= 240 ? i18n::tr("Search name, vendor, category, format") : i18n::tr("Search plugins"), ms.search,
                           sizeof ms.search);
  const bool active = ImGui::IsItemActive();
  ImGui::PopStyleVar(2);
  ui::FocusRing();
  if (active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ms.search[0] = 0;
  ui::drawIcon(dl, ui::Icon::Search, ImVec2(p.x + 14, p.y + h * 0.5f), 12, P.textFaint);
  if (ms.search[0]) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - h + 2, p.y + 2));
    if (ui::IconButton("##clear", ui::Icon::Close, false, h - 4, "Clear the search (Esc)")) ms.search[0] = 0;
  }
  (void)a;
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, h));
}

// Table header row, taller than ImGui's default and on the panel colour.
void headersRow(int columns) {
  const float h = 26;
  ImGui::TableNextRow(ImGuiTableRowFlags_Headers, h);
  for (int c = 0; c < columns; ++c) {
    if (!ImGui::TableSetColumnIndex(c)) continue;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFontSize()) * 0.5f - 1);
    ImGui::PushID(c);
    ImGui::TableHeader(ImGui::TableGetColumnName(c));
    ImGui::PopID();
  }
}

constexpr float kSlotW = 38;  // one format tag in the table

// Format tags of a row in the platform's format order; the listed one lit.
void formatSlots(App& a, ImDrawList* dl, ImVec2 p, float rowH, const host::GroupSnapshot& s, const host::PluginGroup& g, bool dim) {
  const float h = 17;
  float x = p.x;
  for (const host::Format& f : host::formats()) {
    bool has = false, pref = false;
    for (size_t m : g.members)
      if (s.items[m].format == f.id) {
        has = true;
        pref = pref || m == g.preferred;
      }
    if (!has) continue;
    badge(dl, a.fontMono, ImVec2(x, p.y + (rowH - h) * 0.5f), h, f.label, pref && !dim && g.members.size() > 1, dim, kSlotW - 4);
    x += kSlotW;
  }
}

// Width of the formats column: the most formats any plugin comes in.
float formatSlotsWidth(const Model& md) {
  size_t n = 1;
  for (const Row& r : md.rows)
    if (r.g) {
      std::set<std::string> f;
      for (size_t m : r.g->members) f.insert(md.snap->items[m].format);
      n = std::max(n, f.size());
    }
  return static_cast<float>(n) * kSlotW - 4;
}

void pluginTable(App& a, Model& md, host::PluginPrefs& prefs, bool& changed, ImVec2 size) {
  const auto& P = pal();
  const Colors C = colors();
  ManagerState& ms = state();
  const host::GroupSnapshot& s = *md.snap;
  if (md.rows.empty()) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), C.panel, theme::kRadiusLg);
    const bool searching = ms.search[0] != 0;
    const char* title = searching ? "No plugins match your search" : ms.view == kFavourites ? "No favourites yet" : "No plugins found";
    const char* body = searching ? "Try another word, or clear the search." : ms.view == kFavourites ? "Click the star of a plugin to keep it at hand."
                                                                                                   : "Click Find plugins, or add the folder your plugins are in.";
    ui::EmptyState(ImGui::GetWindowDrawList(), p, ImVec2(p.x + size.x, p.y + size.y - (searching ? 40 : 0)), title, body);
    if (searching) {
      const float bw = ui::TextButtonWidth("Clear search");
      ImGui::SetCursorScreenPos(ImVec2(std::round(p.x + (size.x - bw) * 0.5f), p.y + size.y * 0.5f + 34));
      if (ui::TextButton("Clear search", false, bw)) ms.search[0] = 0;
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(size);
    return;
  }
  const float rowH = 30;
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, theme::vec(C.panel));
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, theme::vec(theme::withAlpha(C.panelLine, 0.6f)));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 0));
  const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_PadOuterX;
  const bool narrow = size.x < 560;
  if (ImGui::BeginTable("##plugins", 6, tf, size)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##fav", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize, 18);
    ImGui::TableSetupColumn(i18n::tr("Name"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.5f);
    ImGui::TableSetupColumn(i18n::tr("Formats"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize,
                            formatSlotsWidth(md));
    ImGui::TableSetupColumn(i18n::tr("Category"), ImGuiTableColumnFlags_WidthFixed, narrow ? 76 : 96);
    ImGui::TableSetupColumn(i18n::tr("Vendor"), ImGuiTableColumnFlags_WidthStretch | (narrow ? ImGuiTableColumnFlags_DefaultHide : 0), 1.0f);
    ImGui::TableSetupColumn("##sb", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize, 18);
    headersRow(6);
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsCount > 0) {
      const int col = sp->Specs[0].ColumnIndex;
      const bool asc = sp->Specs[0].SortDirection != ImGuiSortDirection_Descending;
      if (sp->SpecsDirty || col != ms.sortColumn || asc != ms.sortAscending) {
        ms.sortColumn = col;
        ms.sortAscending = asc;
        sp->SpecsDirty = false;
      }
    }
    int selIndex = -1;
    for (size_t i = 0; i < md.rows.size(); ++i)
      if (md.rows[i].g && md.rows[i].key == ms.selected) selIndex = static_cast<int>(i);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(md.rows.size()), rowH);
    if (ms.scrollToSelection && selIndex >= 0) clip.IncludeItemByIndex(selIndex);
    while (clip.Step())
      for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
        Row& row = md.rows[static_cast<size_t>(r)];
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
        ImGui::PushID(row.key.c_str());
        ImGui::TableSetColumnIndex(0);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (!row.g) {  // Gridline Suite group header
          ImVec2 p0 = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable("##suitehdr", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowH))) ms.suiteOpen = !ms.suiteOpen;
          const bool hov = ImGui::IsItemHovered();
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hov ? theme::mix(C.panel, P.text, 0.05f) : C.panel);
          if (hov) ui::Tooltip("Gridline's own plugins as VST3 / CLAP / AU builds for other applications. Gridline uses the built-in versions, so the browser does not list these.");
          ui::drawIcon(dl, ms.suiteOpen ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(p0.x + 8, p0.y + rowH * 0.5f), 10, P.textDim);
          ImGui::TableSetColumnIndex(1);
          const ImVec2 p1 = ImGui::GetCursorScreenPos();
          const std::string t = fmtCount(i18n::N_("Gridline Suite builds (%d)"), md.suiteCount);
          text(dl, a.fontBold, theme::kFontCaption, ImVec2(p1.x, p1.y + (rowH - theme::kFontCaption) * 0.5f - 1), P.textDim, t.c_str());
          ImGui::TableSetColumnIndex(3);
          const ImVec2 p3 = ImGui::GetCursorScreenPos();
          text(dl, a.fontRegular, theme::kFontLabel, ImVec2(p3.x, p3.y + (rowH - theme::kFontLabel) * 0.5f - 1), P.textFaint, i18n::tr("Built in"),
               ImGui::GetContentRegionAvail().x);
          if (ImGui::TableSetColumnIndex(4)) {
            const ImVec2 p4 = ImGui::GetCursorScreenPos();
            text(dl, a.fontRegular, theme::kFontLabel, ImVec2(p4.x, p4.y + (rowH - theme::kFontLabel) * 0.5f - 1), P.textFaint,
                 i18n::tr("Not listed in the browser"), ImGui::GetContentRegionAvail().x);
          }
          ImGui::PopID();
          continue;
        }
        const host::PluginGroup& g = *row.g;
        const bool sel = row.key == ms.selected;
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) {
          ms.selected = row.key;
        }
        const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addToProject(a, s.items[g.preferred].type, g.instrument);
        if (ImGui::BeginDragDropSource()) {
          const std::string& type = s.items[g.preferred].type;
          ImGui::SetDragDropPayload("GL_PLUGIN", type.data(), type.size());
          ui::DragPreview(g.instrument ? ui::Icon::Plug : ui::Icon::Mixer, g.name.c_str());
          ImGui::EndDragDropSource();
        }
        if (sel) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, C.rowSel);
          dl->AddRectFilled(ImVec2(c0.x - 8, c0.y + 5), ImVec2(c0.x - 6, c0.y + rowH - 5), C.selBar, 1.0f);
        } else if (hov) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, C.rowHover);
        }
        if (sel && ms.scrollToSelection) {
          ImGui::SetScrollHereY(0.5f);
          ms.scrollToSelection = false;
        }
        const bool dim = g.ownSuite;
        // star (Suite builds are not offered as favourites: Gridline uses the built-in plugins)
        if (!g.ownSuite) {
          ImGui::SetCursorScreenPos(ImVec2(c0.x - 2, c0.y + (rowH - 20) * 0.5f));
          if (ImGui::InvisibleButton("##fav", ImVec2(22, 20))) {
            setFavourite(s, g, prefs, !row.fav);
            row.fav = !row.fav;
            changed = true;
          }
          const bool starHov = ImGui::IsItemHovered();
          if (starHov) ui::Tooltip(row.fav ? "Favourite: listed first in the browser (click to remove)" : "Add to favourites");
          const ImVec2 sc(c0.x + 8, c0.y + rowH * 0.5f);
          if (row.fav) ui::drawIcon(dl, ui::Icon::StarFilled, sc, 13, theme::lit(P.hot));
          else if (hov || starHov || sel) ui::drawIcon(dl, ui::Icon::Star, sc, 13, starHov ? P.text : P.textFaint);
        }
        // name with the category glyph
        ImGui::TableSetColumnIndex(1);
        const ImVec2 n0 = ImGui::GetCursorScreenPos();
        const float nw = ImGui::GetContentRegionAvail().x;
        const ImU32 role = theme::litRole(browserui::categoryRole(row.category, static_cast<int>(g.instrument ? PluginKind::Generator : PluginKind::Effect)));
        browserui::drawCategoryGlyph(dl, row.category, static_cast<int>(g.instrument ? PluginKind::Generator : PluginKind::Effect),
                                     ImVec2(n0.x + 8, n0.y + rowH * 0.5f), 14, dim ? P.textFaint : role);
        text(dl, a.fontRegular, ImGui::GetFontSize(), ImVec2(n0.x + 22, n0.y + (rowH - ImGui::GetFontSize()) * 0.5f - 1), dim ? P.textDim : P.text,
             g.name.c_str(), nw - 24);
        // formats
        ImGui::TableSetColumnIndex(2);
        formatSlots(a, dl, ImGui::GetCursorScreenPos(), rowH, s, g, dim);
        // category
        ImGui::TableSetColumnIndex(3);
        {
          const ImVec2 p = ImGui::GetCursorScreenPos();
          const bool generic = host::isGenericCategory(row.category);
          text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x, p.y + (rowH - theme::kFontCaption) * 0.5f - 1), generic || dim ? P.textFaint : P.textDim,
               i18n::tr(row.category.c_str()), ImGui::GetContentRegionAvail().x);
        }
        // vendor
        if (ImGui::TableSetColumnIndex(4)) {
          const ImVec2 p = ImGui::GetCursorScreenPos();
          text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x, p.y + (rowH - theme::kFontCaption) * 0.5f - 1), dim ? P.textFaint : P.textDim,
               g.vendor.empty() ? i18n::tr("Unknown vendor") : g.vendor.c_str(), ImGui::GetContentRegionAvail().x);
        }
        // sandbox state (switched in the details)
        ImGui::TableSetColumnIndex(5);
        if (row.sandbox) {
          const ImVec2 p = ImGui::GetCursorScreenPos();
          ImGui::Dummy(ImVec2(16, rowH));
          ui::drawIcon(dl, ui::Icon::Lock, ImVec2(p.x + 7, p.y + rowH * 0.5f), 12, row.sandbox == 2 ? theme::lit(P.led) : P.textDim);
          if (ImGui::IsItemHovered())
            ui::Tooltip(row.sandbox == 2 ? "Runs in a sandbox: its own process, so a crash cannot close Gridline"
                                         : "Some formats of this plugin run in a sandbox");
        }
        ImGui::PopID();
      }
    if (selIndex < 0) ms.scrollToSelection = false;
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
}

// Failed plugins whose file name looks like this plugin (another format of it).
std::vector<const host::CacheEntry*> failedFor(const Model& md, const host::PluginGroup& g) {
  std::vector<const host::CacheEntry*> out;
  const std::string key = host::groupNameKey(g.name);
  for (const auto& e : md.failed)
    if (host::groupNameKey(fs::path(e.entry).stem().string()) == key) out.push_back(&e);
  return out;
}

void detailRow(App& a, const char* label, const std::string& value, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float lw = std::min(96.0f, w * 0.38f);
  text(dl, a.fontRegular, theme::kFontCaption, p, P.textFaint, i18n::tr(label), lw - 6);
  text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + lw, p.y), P.text, value.c_str(), w - lw);
  ImGui::Dummy(ImVec2(w, theme::kFontCaption + 6));
}

void detailsPanel(App& a, const Model& md, host::PluginPrefs& prefs, bool& changed) {
  const auto& P = pal();
  const Colors C = colors();
  ManagerState& ms = state();
  const host::GroupSnapshot& s = *md.snap;
  const host::PluginGroup* g = nullptr;
  for (const host::PluginGroup& x : s.groups)
    if (groupKey(x) == ms.selected) g = &x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x;
  if (!g) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    ui::EmptyState(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), "No plugin selected", "Select a plugin to see its details.");
    return;
  }
  const host::HostedClass& c = *s.classes[g->preferred];
  const int kind = static_cast<int>(g->instrument ? PluginKind::Generator : PluginKind::Effect);
  const std::string cat = shownCategory(s, *g, prefs);
  // ---- header: glyph tile, name, vendor + version
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 role = theme::litRole(browserui::categoryRole(cat, kind));
    dl->AddRectFilled(p, ImVec2(p.x + 40, p.y + 40), theme::withAlpha(role, 0.14f), theme::kRadiusLg + 2);
    dl->AddRect(p, ImVec2(p.x + 40, p.y + 40), theme::withAlpha(role, 0.35f), theme::kRadiusLg + 2);
    browserui::drawCategoryGlyph(dl, cat, kind, ImVec2(p.x + 20, p.y + 20), 22, role);
    text(dl, a.fontBold, theme::kFontTitle, ImVec2(p.x + 52, p.y + 1), P.text, g->name.c_str(), w - 52);
    std::string sub = g->vendor.empty() ? std::string(i18n::tr("Unknown vendor")) : g->vendor;
    if (!c.version.empty()) sub += "  \xC2\xB7  " + c.version;
    text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + 52, p.y + 22), P.textDim, sub.c_str(), w - 52);
    ImGui::Dummy(ImVec2(w, 44));
  }
  // ---- tags: kind, category, Suite
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = p.x;
    auto tag = [&](const char* t, bool lit) {
      const float tw = textW(a.fontRegular, theme::kFontLabel, t) + 14;
      if (x + tw > p.x + w) return;
      dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + tw, p.y + 18), lit ? theme::withAlpha(P.hot, 0.14f) : theme::withAlpha(P.text, 0.06f), 9);
      text(dl, a.fontRegular, theme::kFontLabel, ImVec2(x + 7, p.y + 3), lit ? theme::lit(P.hot) : P.textDim, t);
      x += tw + 4;
    };
    tag(i18n::tr(g->instrument ? "Instrument" : "Effect"), false);
    if (!host::isGenericCategory(cat)) tag(i18n::tr(cat.c_str()), false);
    if (g->ownSuite) tag(i18n::tr("Gridline Suite"), true);
    ImGui::Dummy(ImVec2(w, 22));
  }
  // ---- actions (none for a Suite build: Gridline uses the built-in plugin)
  if (!g->ownSuite) {
    const bool fav = groupFav(s, *g, prefs);
    ImGui::Dummy(ImVec2(0, 2));
    const float bw = std::floor((w - 6) * 0.5f);
    if (ui::TextButton(fav ? "Favourite" : "Add to favourites", fav, bw, "Favourites are listed first in the browser")) {
      setFavourite(s, *g, prefs, !fav);
      changed = true;
    }
    ImGui::SameLine(0, 6);
    if (ui::TextButton("Add to project", false, bw, "Instruments: a new channel. Effects: the next free slot of the selected mixer track (double-click a row does the same)"))
      addToProject(a, s.items[g->preferred].type, g->instrument);
  }
  if (g->ownSuite) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushFont(a.fontRegular, theme::kFontCaption);
    ImGui::TextColored(theme::vec(P.textDim), "%s",
                       i18n::tr("A Gridline Suite build of a plugin that is built into Gridline, made for other applications. Use the built-in version in Gridline; the browser does not list these builds."));
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
  }
  // ---- formats: preferred pick, versions, paths
  const bool pickable = g->members.size() > 1 && !g->ownSuite;
  caption(a, i18n::N_("FORMATS"), pickable ? i18n::tr("browser lists") : nullptr);
  const host::ScanStatus ss = host::scanStatus();
  for (size_t m : g->members) {
    const host::HostedClass& mc = *s.classes[m];
    const bool pref = m == g->preferred;
    ImGui::PushID(mc.type.c_str());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rh = 38;
    ImGui::SetNextItemAllowOverlap();
    const bool click = ImGui::InvisibleButton("##fmt", ImVec2(w, rh));
    const bool hov = ImGui::IsItemHovered();
    ui::FocusRing();
    if (click && !pref && pickable) {
      setPreferred(s, *g, prefs, m);
      changed = true;
    }
    if (hov) {
      std::string tip = mc.bundle;
      if (pickable) tip += "\n" + std::string(pref ? i18n::tr("The browser lists this format") : i18n::tr("Click: list this format in the browser"));
      ImGui::SetTooltip("%s", tip.c_str());
    }
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh - 4), pref && pickable ? theme::withAlpha(P.hot, 0.07f) : hov ? C.rowHover : theme::withAlpha(C.field, 0.5f),
                      theme::kRadiusLg);
    if (pref && pickable) dl->AddRect(p, ImVec2(p.x + w, p.y + rh - 4), theme::withAlpha(P.hot, 0.35f), theme::kRadiusLg);
    // radio
    const ImVec2 rc(p.x + 12, p.y + (rh - 4) * 0.5f);
    if (pickable) {
      dl->AddCircle(rc, 5.5f, pref ? theme::lit(P.hot) : P.textFaint, 0, 1.3f);
      if (pref) dl->AddCircleFilled(rc, 3.0f, theme::lit(P.hot));
    }
    const float bx = p.x + (pickable ? 24 : 8);
    const float bw = badge(dl, a.fontMono, ImVec2(bx, p.y + 4), 16, formatLabel(mc.format), pref && pickable, false, 38);
    std::string ver = mc.version.empty() ? std::string() : "v" + mc.version;
    text(dl, a.fontMono, 10.5f, ImVec2(bx + bw + 8, p.y + 6), P.textDim, ver.c_str());
    const std::string path = mc.format == "au" ? std::string("Audio Unit  ") + mc.bundle : tildePath(mc.bundle);
    const std::string shown = fitMiddle(a.fontMono, 10.0f, path, w - (bx - p.x) - 8);
    text(dl, a.fontMono, 10.0f, ImVec2(bx, p.y + 21), P.textFaint, shown.c_str());
    ImGui::PopID();
  }
  if (pickable) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushFont(a.fontRegular, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textFaint), "%s",
                       g->userPicked ? i18n::tr("Picked for this plugin. Projects keep the format they were made with.")
                                     : i18n::tr("From the preferred format setting. Click a format to pick it for this plugin."));
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    if (g->userPicked) {
      if (ui::TextButton("Use the preferred format", false, 0, "Forget the pick: the preferred format setting decides")) {
        for (size_t m : g->members) prefs.preferTypes.erase(s.items[m].type);
        changed = true;
      }
    }
  }
  // ---- scan status of every format
  const auto fails = failedFor(md, *g);
  caption(a, i18n::N_("SCAN"));
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool bad = !fails.empty();
    ui::drawIcon(dl, bad ? ui::Icon::Info : ui::Icon::Check, ImVec2(p.x + 6, p.y + 8), 12, bad ? P.danger : theme::lit(P.led));
    const std::string t = bad ? fmtCount(fails.size() == 1 ? i18n::N_("Scanned; %d other format failed") : i18n::N_("Scanned; %d other formats failed"), static_cast<int>(fails.size()))
                              : std::string(i18n::tr("Scanned without problems"));
    text(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + 18, p.y + 1), bad ? P.danger : P.textDim, t.c_str(), w - 18);
    ImGui::Dummy(ImVec2(w, 18));
    for (const host::CacheEntry* e : fails) {
      const ImVec2 q = ImGui::GetCursorScreenPos();
      badge(dl, a.fontMono, ImVec2(q.x, q.y + 1), 16, formatLabel(e->format), false, false, 38);
      text(dl, a.fontRegular, theme::kFontLabel, ImVec2(q.x + 44, q.y + 2), P.danger, e->error.c_str(), w - 44);
      ImGui::Dummy(ImVec2(w, 20));
    }
  }
  ImGui::BeginDisabled(ss.running);
  if (ui::TextButton(g->members.size() > 1 ? "Rescan this plugin" : "Rescan", false, 0, "Scans every format of this plugin again")) {
    std::vector<std::pair<std::string, std::string>> todo;
    for (size_t m : g->members) todo.push_back({s.classes[m]->format, s.classes[m]->bundle});
    for (const host::CacheEntry* e : fails) todo.push_back({e->format, e->entry});
    host::rescanEntries(host::selfExecutablePath(), todo);
  }
  ImGui::EndDisabled();
  // ---- sandbox
  caption(a, i18n::N_("SAFETY"));
  {
    const int sb = groupSandbox(s, *g, prefs);
    bool on = sb == 2;
    if (ImGui::Checkbox(i18n::tr("Run in a sandbox"), &on)) setSandbox(a, s, *g, on || sb == 1);
    if (sb == 1) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("(some formats)"));
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushFont(a.fontRegular, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Runs the plugin in its own process: if it crashes, it goes silent instead of closing Gridline. Uses a little more CPU."));
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
  }
  // ---- category override
  caption(a, i18n::N_("CATEGORY"));
  {
    const std::string key = groupKey(*g);
    if (ms.categoryFor != key) {
      ms.categoryFor = key;
      std::string own;
      for (size_t m : g->members)
        if (auto it = prefs.categories.find(s.items[m].type); it != prefs.categories.end() && own.empty()) own = it->second;
      std::snprintf(ms.category, sizeof ms.category, "%s", own.c_str());
    }
    ImGui::SetNextItemWidth(w);
    const std::string hint = std::string(i18n::tr(g->category.c_str())) + "  " + i18n::tr("(type to change)");
    if (ImGui::InputTextWithHint("##cat", hint.c_str(), ms.category, sizeof ms.category)) {
      for (size_t m : g->members) {
        if (ms.category[0]) prefs.categories[s.items[m].type] = ms.category;
        else prefs.categories.erase(s.items[m].type);
      }
      changed = true;
    }
  }
  // ---- what the plugin offers
  caption(a, i18n::N_("DETAILS"));
  detailRow(a, i18n::N_("Parameters"), std::to_string(c.params.size()), w);
  if (!c.programs.empty()) detailRow(a, i18n::N_("Presets"), std::to_string(c.programs.size()), w);
  detailRow(a, i18n::N_("Outputs"), c.auxOutputs.empty() ? std::string(i18n::tr("Stereo")) : std::to_string(c.auxOutputs.size() + 1), w);
  if (c.sidechain) detailRow(a, i18n::N_("Sidechain"), i18n::tr("Yes"), w);
  if (c.eventInput || c.eventOutput)
    detailRow(a, i18n::N_("MIDI"), c.eventInput && c.eventOutput ? i18n::tr("In and out") : c.eventInput ? i18n::tr("In") : i18n::tr("Out"), w);
  detailRow(a, i18n::N_("Plugin id"), c.type, w);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.type.c_str());
}

void failedDetails(App& a, const Model& md) {
  const auto& P = pal();
  ManagerState& ms = state();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x;
  const host::CacheEntry* e = nullptr;
  for (const auto& f : md.failed)
    if (f.format + "\n" + f.entry == ms.selectedFail) e = &f;
  if (!e) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    ui::EmptyState(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), "Failed plugins", "Nothing failed to scan.");
    return;
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const std::string name = e->format == "au" ? e->entry : fs::path(e->entry).stem().string();
  text(dl, a.fontBold, theme::kFontTitle, p, P.text, name.c_str(), w);
  ImGui::Dummy(ImVec2(w, 22));
  const ImVec2 q = ImGui::GetCursorScreenPos();
  badge(dl, a.fontMono, q, 16, formatLabel(e->format), false, false, 38);
  ImGui::Dummy(ImVec2(w, 22));
  caption(a, i18n::N_("REASON"));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
  ImGui::TextColored(theme::vec(P.danger), "%s", e->error.empty() ? i18n::tr("Unknown error") : e->error.c_str());
  ImGui::PopTextWrapPos();
  caption(a, i18n::N_("LOCATION"));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textDim), "%s", tildePath(e->entry).c_str());
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS2));
  ImGui::BeginDisabled(host::scanStatus().running);
  if (ui::TextButton("Retry", true, 0, "Scan this plugin again")) host::rescanEntry(host::selfExecutablePath(), e->format, e->entry);
  ImGui::EndDisabled();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
  ImGui::PushFont(a.fontRegular, theme::kFontLabel);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Every scan runs in its own process, so a plugin that crashes here cannot take Gridline down."));
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
}

void failedTable(App& a, Model& md, ImVec2 size) {
  const auto& P = pal();
  const Colors C = colors();
  ManagerState& ms = state();
  const std::string q = lower(ms.search);
  std::vector<const host::CacheEntry*> rows;
  for (const auto& e : md.failed)
    if (q.empty() || lower(e.entry).find(q) != std::string::npos || lower(e.error).find(q) != std::string::npos) rows.push_back(&e);
  if (rows.empty()) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), C.panel, theme::kRadiusLg);
    ui::EmptyState(ImGui::GetWindowDrawList(), p, ImVec2(p.x + size.x, p.y + size.y), md.failed.empty() ? "No failed plugins" : "No failed plugins match",
                   md.failed.empty() ? "Every plugin scanned cleanly." : "Try another word, or clear the search.");
    ImGui::Dummy(size);
    return;
  }
  const bool busy = host::scanStatus().running;
  const float rowH = 42;
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, theme::vec(C.panel));
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, theme::vec(theme::withAlpha(C.panelLine, 0.6f)));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 0));
  if (ImGui::BeginTable("##failed", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX, size)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(i18n::tr("Format"), ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn(i18n::tr("Plugin and reason"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("##retry", ImGuiTableColumnFlags_WidthFixed, ui::TextButtonWidth("Retry"));
    headersRow(3);
    for (const host::CacheEntry* e : rows) {
      const std::string id = e->format + "\n" + e->entry;
      ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
      ImGui::PushID(id.c_str());
      ImGui::TableSetColumnIndex(0);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      const bool sel = ms.selectedFail == id;
      if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) ms.selectedFail = id;
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
      if (sel) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, C.rowSel);
        dl->AddRectFilled(ImVec2(c0.x - 8, c0.y + 6), ImVec2(c0.x - 6, c0.y + rowH - 6), C.selBar, 1.0f);
      } else if (hov) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, C.rowHover);
      }
      if (sel && ms.scrollToSelection) {
        ImGui::SetScrollHereY(0.5f);
        ms.scrollToSelection = false;
      }
      badge(dl, a.fontMono, ImVec2(c0.x, c0.y + (rowH - 16) * 0.5f), 16, formatLabel(e->format), false, false, 40);
      ImGui::TableSetColumnIndex(1);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const std::string name = e->format == "au" ? e->entry : fs::path(e->entry).stem().string();
      text(dl, a.fontRegular, ImGui::GetFontSize(), ImVec2(p.x, p.y + 5), P.text, name.c_str(), w);
      text(dl, a.fontRegular, theme::kFontLabel, ImVec2(p.x, p.y + 23), P.danger, e->error.empty() ? i18n::tr("Unknown error") : e->error.c_str(), w);
      ImGui::TableSetColumnIndex(2);
      const ImVec2 bp = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
      ImGui::BeginDisabled(busy);
      if (ui::TextButton("Retry", false, 0, "Scan this plugin again")) host::rescanEntry(host::selfExecutablePath(), e->format, e->entry);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
}

// Up / Down / Page / Home / End move the selection when no text field has the keyboard.
void keyboard(Model& md) {
  ManagerState& ms = state();
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
  const ImGuiIO& io = ImGui::GetIO();
  if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_F, false)) ms.focusSearch = true;
  if (io.WantTextInput) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ms.search[0]) ms.search[0] = 0;
  // Selectable keys of the current view, in row order.
  std::vector<std::string> keys;
  if (ms.view == kFailed) {
    for (const auto& e : md.failed) keys.push_back(e.format + "\n" + e.entry);
  } else {
    for (const Row& r : md.rows)
      if (r.g) keys.push_back(r.key);
  }
  if (keys.empty()) return;
  std::string& selected = ms.view == kFailed ? ms.selectedFail : ms.selected;
  int cur = -1;
  for (size_t k = 0; k < keys.size(); ++k)
    if (keys[k] == selected) cur = static_cast<int>(k);
  int next = cur;
  const int n = static_cast<int>(keys.size());
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next = cur < 0 ? 0 : std::min(n - 1, cur + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next = cur < 0 ? 0 : std::max(0, cur - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) next = std::min(n - 1, std::max(0, cur) + 12);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) next = std::max(0, cur - 12);
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) next = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_End, false)) next = n - 1;
  if (next != cur && next >= 0) {
    selected = keys[static_cast<size_t>(next)];
    ms.scrollToSelection = true;
  }
}

}  // namespace

void drawPluginManager(App& a) {
  if (!a.showPluginManager) return;
  ManagerState& ms = state();
  const Colors C = colors();
  ui::PlaceNextDialog(ImVec2(1120, 680), ImGuiCond_Appearing, ImVec2(720, 420));  // J-54: fits short / scaled screens
  if (!ImGui::Begin(ui::windowTitle("Plugin manager").c_str(), &a.showPluginManager, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  host::PluginPrefs prefs = host::pluginPrefs();
  bool changed = false;
  const std::string q = lower(ms.search);
  Model md = buildModel(prefs, q);
  // Keep a selection: the first row when nothing (or something no longer shown) is selected.
  if (ms.view != kFailed) {
    bool shown = false;
    for (const Row& r : md.rows) shown = shown || (r.g && r.key == ms.selected);
    if (!shown) ms.selected = !md.rows.empty() && md.rows.front().g ? md.rows.front().key : std::string();
  } else {
    bool shown = false;
    for (const auto& e : md.failed) shown = shown || e.format + "\n" + e.entry == ms.selectedFail;
    if (!shown && !md.failed.empty()) ms.selectedFail = md.failed.front().format + "\n" + md.failed.front().entry;
  }
  keyboard(md);

  const float winW = ImGui::GetContentRegionAvail().x;
  const float leftW = winW < 900 ? 228.0f : 256.0f;

  // ---- left: scan, formats, folders (a recessed panel) ----
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(C.panel));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusLg + 2);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
  ImGui::BeginChild("##left", ImVec2(leftW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  leftColumn(a, md, prefs, changed);
  ImGui::EndChild();
  ImGui::SameLine(0, 12);

  // ---- right: tabs + search, table, details ----
  ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const float rightW = ImGui::GetContentRegionAvail().x;
  const bool side = rightW >= 760;  // details beside the table; below it in a narrow window
  const float searchW = std::clamp(rightW * 0.3f, 150.0f, 280.0f);
  viewTabs(a, md, rightW - searchW - 10);
  ImGui::SameLine(0, 0);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(10.0f, ImGui::GetContentRegionAvail().x - searchW));
  searchBox(a, searchW);
  ImGui::Dummy(ImVec2(0, 2));
  const float availH = ImGui::GetContentRegionAvail().y;
  const float detW = side ? std::clamp(rightW * 0.32f, 270.0f, 330.0f) : rightW;
  const float detH = side ? availH : std::min(240.0f, std::max(150.0f, availH * 0.42f));
  const ImVec2 tableSize(side ? rightW - detW - 10 : rightW, side ? availH : std::max(120.0f, availH - detH - 8));
  if (ms.view == kFailed) failedTable(a, md, tableSize);
  else pluginTable(a, md, prefs, changed, tableSize);
  if (side) ImGui::SameLine(0, 10);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(C.panel));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusLg + 2);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
  ImGui::BeginChild("##detail", ImVec2(detW, detH), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (ms.view == kFailed) failedDetails(a, md);
  else detailsPanel(a, md, prefs, changed);
  ImGui::EndChild();
  ImGui::EndChild();
  if (changed) {
    // Sandbox switches were saved by setHostedSandbox: keep them.
    host::PluginPrefs now = host::pluginPrefs();
    prefs.sandbox = now.sandbox;
    host::setPluginPrefs(prefs);
  }
  ImGui::End();
}

}  // namespace gl

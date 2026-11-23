// Gridline: the Patch Player's editor. A header that always shows the file, the patch (with
// arrows and its whole list one click away) and two tabs:
//   Patches  banks, Starred and Recent; a search that also matches families; family chips; the
//            patch list with program numbers and zone counts; what the patch is made of; a
//            keyboard showing its zones (play it to hear the patch)
//   Sound    output with sends, pitch, voices, filter offsets, the volume envelope (from the
//            patch, scaled or replaced), vibrato, and what the file itself asks for
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/ExportUi.h"
#include "../../app/LayoutPresets.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/TrackerEditors.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Platform.h"
#include "patchplayer.h"
#include "patchplayer_ui.h"

namespace gl::packs::samplers {

using theme::pal;
namespace fs = std::filesystem;

namespace {

constexpr float kW = 1028, kH = 584;
// The Sound tab's rows end 370 px down: the window is shorter there, so the keyboard sits right
// under them (14 px apart) instead of 120 px lower.
constexpr float kHSound = 468;

// The patch families (patchFamilies() in patchplayer_dsp.cpp), marked for translation here.
[[maybe_unused]] constexpr const char* kFamilyNames[] = {
    i18n::N_("Piano"), i18n::N_("Chromatic"), i18n::N_("Organ"), i18n::N_("Guitar"), i18n::N_("Bass"), i18n::N_("Strings"),
    i18n::N_("Ensemble"), i18n::N_("Brass"), i18n::N_("Reed"), i18n::N_("Pipe"), i18n::N_("Synth lead"), i18n::N_("Synth pad"),
    i18n::N_("Synth effects"), i18n::N_("World"), i18n::N_("Percussive"), i18n::N_("Sound effects"), i18n::N_("Drum kits")};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string noteName(int n) {
  static const char* nm[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  n = std::clamp(n, 0, 127);
  return std::string(nm[n % 12]) + std::to_string(n / 12 - 1);
}
bool isBlack(int n) {
  const int pc = n % 12;
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
std::string bytesText(size_t b) {
  char t[32];
  if (b >= (1u << 20)) std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(b) / (1 << 20));
  else std::snprintf(t, sizeof t, "%.0f KB", std::ceil(static_cast<double>(b) / 1024.0));
  return t;
}
std::string bankLabel(int bank) {
  char t[64];
  std::snprintf(t, sizeof t, "Bank %03d \xC2\xB7 %s", bank, bank == 0 ? i18n::tr("Melodic") : bank >= 120 ? i18n::tr("Drum kits") : i18n::tr("Variations"));
  return t;
}

// ------------------------------------------------------------------ per-person lists
// Starred and Recent patches: "<path>|<bank>|<program>" per line, kept in the settings (never in
// a project, never synced).
std::vector<std::string> readList(App& a, const char* key) {
  std::vector<std::string> out;
  std::string s = a.settings.getString(key), line;
  for (char c : s) {
    if (c == '\n') {
      if (!line.empty()) out.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) out.push_back(line);
  return out;
}
void writeList(App& a, const char* key, const std::vector<std::string>& v) {
  std::string s;
  for (const auto& l : v) s += l + "\n";
  a.settings.set(key, s);
}
std::string listKey(const std::string& path, int bank, int program) { return path + "|" + std::to_string(bank) + "|" + std::to_string(program); }
bool parseKey(const std::string& k, std::string& path, int& bank, int& program) {
  const size_t b2 = k.rfind('|');
  if (b2 == std::string::npos || b2 == 0) return false;
  const size_t b1 = k.rfind('|', b2 - 1);
  if (b1 == std::string::npos) return false;
  path = k.substr(0, b1);
  bank = std::atoi(k.c_str() + b1 + 1);
  program = std::atoi(k.c_str() + b2 + 1);
  return true;
}
void pushRecent(App& a, const std::string& k) {
  auto v = readList(a, "patchplayer.recent");
  v.erase(std::remove(v.begin(), v.end(), k), v.end());
  v.insert(v.begin(), k);
  if (v.size() > 40) v.resize(40);
  writeList(a, "patchplayer.recent", v);
}
bool isStarred(App& a, const std::string& k) {
  auto v = readList(a, "patchplayer.starred");
  return std::find(v.begin(), v.end(), k) != v.end();
}
void toggleStar(App& a, const std::string& k) {
  auto v = readList(a, "patchplayer.starred");
  auto it = std::find(v.begin(), v.end(), k);
  if (it != v.end()) v.erase(it);
  else v.push_back(k);
  writeList(a, "patchplayer.starred", v);
}
void pushRecentFile(App& a, const std::string& path) {
  auto v = readList(a, "patchplayer.files");
  v.erase(std::remove(v.begin(), v.end(), path), v.end());
  v.insert(v.begin(), path);
  if (v.size() > 12) v.resize(12);
  writeList(a, "patchplayer.files", v);
}

// ------------------------------------------------------------------ editor state
struct Ui {
  int tab = 0;         // 0 Patches, 1 Sound
  int place = 0;       // 0 a bank, 1 Starred, 2 Recent
  int bank = -1;       // the bank shown (-1: the patch's own)
  char find[64] = {};
  std::string family;  // a family chip (empty: all)
  int kbKey = -1;      // key held on the keyboard strip
  int previewKey = -1;
  double previewOff = 0;
  bool scrollToCurrent = true;
  int devPick = -1;    // dev input: pick this row of the list
};
std::map<uint64_t, Ui> gUi;

struct Row {
  std::string path;
  int preset = -1;  // index into the file's presets
  int bank = 0, program = 0;
  std::string name;
  int zones = 0;
};

const PluginState* stateOf(App& a, const PluginTarget& t) { return t.state(a.doc.song()); }

// One undo step: the channel plays this patch (and its own sends to start with).
void choosePatch(App& a, const PluginTarget& t, const std::string& path, int bank, int program, const std::string& name) {
  const PluginState* cur = stateOf(a, t);
  if (!cur) return;
  PluginState ns = *cur;
  setPatch(ns, path, bank, program, name);
  if (ns.data == cur->data && ns.params == cur->params) return;
  setPluginState(a, t, ns, "change patch");
  pushRecent(a, listKey(path, bank, program));
  pushRecentFile(a, path);
}

void previewNote(App& a, const PluginTarget& t, Ui& ui, int key) {
  if (!t.isChannel()) return;
  if (ui.previewKey >= 0) a.engine.noteOff(t.owner, ui.previewKey);
  a.engine.noteOn(t.owner, key, 0.8f);
  ui.previewKey = key;
  ui.previewOff = ImGui::GetTime() + 0.6;
}

void openFileDialog(App& a, const PluginTarget& t) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::tr("Open an SF2 file");
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = {".sf2", ".sf3"};
  PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& p) {
    std::string err;
    auto f = loadSf2(p, &err);
    if (!f) {
      a.notify(err.empty() ? std::string(i18n::tr("Cannot read the file: ")) + p : err, true);
      return;
    }
    const auto& pr = f->presets.front();
    choosePatch(a, tt, p, pr.bank, pr.program, pr.name);
  };
}

// ------------------------------------------------------------------ drawing helpers
void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* caption = nullptr) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface800, 6);
  dl->AddRect(p0, p1, theme::mix(P.surface800, P.text, 0.08f), 6);
  if (title) {
    ImGui::PushFont(app().fontMonoBold, 10.5f);
    dl->AddText(ImVec2(p0.x + 10, p0.y + 8), P.textDim, title);
    if (caption) {
      ImGui::PopFont();
      ImGui::PushFont(nullptr, 11.0f);
      const ImVec2 cs = ImGui::CalcTextSize(caption);
      dl->AddText(ImVec2(p1.x - 10 - cs.x, p0.y + 8), P.textFaint, caption);
    }
    ImGui::PopFont();
  }
}
void smallCaps(ImDrawList* dl, ImVec2 p, const char* s) {
  ImGui::PushFont(app().fontMonoBold, 10.5f);
  dl->AddText(p, pal().textFaint, s);
  ImGui::PopFont();
}
// A flat two- or three-way switch over a choice parameter (one undo step per change), its right
// edge at rightTop.x, each cell as wide as the longest label.
void switchRow(App& a, const PluginTarget& t, int param, ImVec2 rightTop, const std::vector<const char*>& labels) {
  float cell = 0;
  for (const char* l : labels) cell = std::max(cell, ImGui::CalcTextSize(l).x + 18);
  const float width = std::ceil(cell) * static_cast<float>(labels.size());
  ImGui::SetCursorScreenPos(ImVec2(rightTop.x - width, rightTop.y));
  pui::segmented(a, t, param, width, labels.data());
}

// The keyboard strip: keys lo..hi, the patch's zones as coloured bars above, a click plays.
void keyboard(App& a, const PluginTarget& t, Ui& ui, const PatchInfo* pi, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int lo = 12, hi = 108;
  int whites = 0;
  for (int k = lo; k <= hi; ++k) whites += !isBlack(k);
  const float ww = w / static_cast<float>(whites), barH = 6;
  const ImVec2 k0(p0.x, p0.y + barH);
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + barH), P.surface950);
  static const ImU32 cols[] = {IM_COL32(74, 144, 196, 255), IM_COL32(56, 160, 140, 255), IM_COL32(132, 104, 196, 255), IM_COL32(196, 150, 70, 255)};
  auto keyX = [&](int key) {
    int wi = 0;
    for (int k = lo; k < key; ++k) wi += !isBlack(k);
    return k0.x + static_cast<float>(wi) * ww - (isBlack(key) ? ww * 0.3f : 0.f);
  };
  if (pi)
    for (size_t i = 0; i < pi->ranges.size(); ++i) {
      const int a0 = std::max(lo, pi->ranges[i].first), a1 = std::min(hi, pi->ranges[i].second);
      if (a1 < a0) continue;
      const float x0 = keyX(a0), x1 = keyX(a1) + (isBlack(a1) ? ww * 0.6f : ww);
      dl->AddRectFilled(ImVec2(x0 + 1, p0.y), ImVec2(x1 - 1, p0.y + barH - 1), cols[i % 4], 2);
    }
  ImGui::SetCursorScreenPos(k0);
  ImGui::InvisibleButton("##ppkeys", ImVec2(w, h - barH));
  int over = -1;
  const ImVec2 m = ImGui::GetIO().MousePos;
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    for (int k = lo; k <= hi && over < 0; ++k)  // black keys first (they sit on top)
      if (isBlack(k) && m.x >= keyX(k) && m.x < keyX(k) + ww * 0.6f && m.y < k0.y + (h - barH) * 0.62f) over = k;
    for (int k = lo; k <= hi && over < 0; ++k)
      if (!isBlack(k) && m.x >= keyX(k) && m.x < keyX(k) + ww) over = k;
  }
  if (ImGui::IsItemActive() && over >= 0 && over != ui.kbKey && t.isChannel()) {
    if (ui.kbKey >= 0) a.engine.noteOff(t.owner, ui.kbKey);
    a.engine.noteOn(t.owner, over, 0.8f);
    ui.kbKey = over;
  }
  if (!ImGui::IsItemActive() && ui.kbKey >= 0) {
    if (t.isChannel()) a.engine.noteOff(t.owner, ui.kbKey);
    ui.kbKey = -1;
  }
  if (ImGui::IsItemHovered() && over >= 0) a.setHint(noteName(over) + ": " + i18n::tr("click or drag to play"));
  auto inZone = [&](int k) {
    if (!pi) return false;
    for (const auto& r : pi->ranges)
      if (k >= r.first && k <= r.second) return true;
    return false;
  };
  const float kh = h - barH;
  for (int k = lo; k <= hi; ++k) {
    if (isBlack(k)) continue;
    const float x = keyX(k);
    const bool lit = k == ui.kbKey || k == ui.previewKey;
    ImU32 c = lit ? P.accent : inZone(k) ? P.keyWhite : theme::mix(P.keyWhite, P.surface800, 0.35f);
    dl->AddRectFilled(ImVec2(x + 0.5f, k0.y), ImVec2(x + ww - 0.5f, k0.y + kh), c, 2, ImDrawFlags_RoundCornersBottom);
  }
  for (int k = lo; k <= hi; ++k) {
    if (!isBlack(k)) continue;
    const float x = keyX(k);
    const bool lit = k == ui.kbKey || k == ui.previewKey;
    dl->AddRectFilled(ImVec2(x, k0.y), ImVec2(x + ww * 0.6f, k0.y + kh * 0.62f), lit ? P.accent : P.keyBlack, 1.5f);
  }
}

// ------------------------------------------------------------------ Patches tab
void patchesTab(App& a, const PluginTarget& t, Ui& ui, const PatchRef& ref, const Sf2File* f, int cur, ImVec2 o) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // ---- left: banks and lists
  const float lx = o.x, ly = o.y, lw = 210, lh = 420;
  dl->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), theme::mix(P.surface900, P.surface800, 0.5f));
  smallCaps(dl, ImVec2(lx + 10, ly + 8), i18n::tr("BANKS"));
  std::vector<std::pair<int, int>> banks;  // bank, count
  if (f)
    for (const auto& p : f->presets) {
      if (banks.empty() || banks.back().first != p.bank) banks.emplace_back(p.bank, 0);
      ++banks.back().second;
    }
  const int shownBank = ui.bank >= 0 ? ui.bank : (cur >= 0 && f ? f->presets[static_cast<size_t>(cur)].bank : (banks.empty() ? 0 : banks[0].first));
  float y = ly + 26;
  auto sideRow = [&](const char* id, const std::string& label, int count, bool on) {
    ImGui::SetCursorScreenPos(ImVec2(lx + 4, y));
    const bool hit = ImGui::InvisibleButton(id, ImVec2(lw - 8, 24));
    const bool hov = ImGui::IsItemHovered();
    if (on || hov) dl->AddRectFilled(ImVec2(lx + 4, y), ImVec2(lx + lw - 4, y + 24), on ? theme::mix(P.surface700, P.accent, 0.12f) : P.surface700, 4);
    ImGui::PushFont(nullptr, 12.5f);
    dl->AddText(ImVec2(lx + 14, y + 5), on ? P.text : P.textDim, label.c_str());
    if (count >= 0) {
      const std::string n = std::to_string(count);
      const ImVec2 ns = ImGui::CalcTextSize(n.c_str());
      dl->AddText(ImVec2(lx + lw - 14 - ns.x, y + 5), P.textFaint, n.c_str());
    }
    ImGui::PopFont();
    y += 26;
    return hit;
  };
  for (const auto& [b, n] : banks) {
    ImGui::PushID(b);
    if (sideRow("##bank", bankLabel(b), n, ui.place == 0 && b == shownBank)) ui.place = 0, ui.bank = b, ui.family.clear();
    ImGui::PopID();
  }
  y += 10;
  smallCaps(dl, ImVec2(lx + 10, y), i18n::tr("LISTS"));
  y += 18;
  const auto starred = readList(a, "patchplayer.starred");
  const auto recent = readList(a, "patchplayer.recent");
  if (sideRow("##starred", std::string("\xE2\x98\x85  ") + i18n::tr("Starred"), static_cast<int>(starred.size()), ui.place == 1)) ui.place = 1, ui.family.clear();
  if (sideRow("##recent", std::string("\xE2\x86\xBA  ") + i18n::tr("Recent"), static_cast<int>(recent.size()), ui.place == 2)) ui.place = 2, ui.family.clear();
  if (sideRow("##other", std::string("\xE2\x96\xA1  ") + i18n::tr("Other files..."), -1, false)) openFileDialog(a, t);
  // ---- the rows of the place chosen
  std::vector<Row> rows;
  auto addFrom = [&](const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
      Row r;
      if (!parseKey(k, r.path, r.bank, r.program)) continue;
      auto g = requestSf2(r.path);
      if (!g) continue;
      r.preset = sf2FindPreset(*g, r.bank, r.program);
      if (r.preset < 0) continue;
      r.name = g->presets[static_cast<size_t>(r.preset)].name;
      r.zones = static_cast<int>(sf2PresetRegions(*g, static_cast<size_t>(r.preset)).size());
      rows.push_back(r);
    }
  };
  static std::map<const Sf2File*, std::vector<int>> zoneCounts;  // per file: zones of each preset (computed once)
  if (ui.place == 0 && f) {
    auto& zc = zoneCounts[f];
    if (zc.size() != f->presets.size()) {
      zc.clear();
      for (size_t i = 0; i < f->presets.size(); ++i) zc.push_back(static_cast<int>(sf2PresetRegions(*f, i).size()));
    }
    for (size_t i = 0; i < f->presets.size(); ++i) {
      const auto& p = f->presets[i];
      if (p.bank != shownBank) continue;
      rows.push_back(Row{ref.path, static_cast<int>(i), p.bank, p.program, p.name, zc[i]});
    }
  } else if (ui.place == 1) {
    addFrom(starred);
  } else if (ui.place == 2) {
    addFrom(recent);
  }
  // ---- middle: search, family chips, list
  const float mx = lx + lw + 12, mw = 560;
  ImGui::SetCursorScreenPos(ImVec2(mx, ly + 6));
  ImGui::SetNextItemWidth(mw);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28, 6));
  ImGui::InputTextWithHint("##ppfind", i18n::tr("name, number or family"), ui.find, sizeof ui.find);
  ImGui::PopStyleVar();
  ui::drawIcon(dl, ui::Icon::Search, ImVec2(mx + 14, ly + 6 + ImGui::GetFrameHeight() * 0.5f + 3), 12, P.textDim);
  const std::string want = lower(ui.find);
  // Families present in these rows.
  std::vector<std::string> fams;
  for (const auto& r : rows) {
    std::string fm = patchFamily(r.bank, r.program);
    if (std::find(fams.begin(), fams.end(), fm) == fams.end()) fams.push_back(fm);
  }
  std::sort(fams.begin(), fams.end(), [](const std::string& x, const std::string& z) {
    const auto& all = patchFamilies();
    return std::find(all.begin(), all.end(), x) < std::find(all.begin(), all.end(), z);
  });
  float cx = mx, cy = ly + 42;
  ImGui::PushFont(nullptr, 12.0f);
  for (const auto& fm : fams) {
    const float tw = ImGui::CalcTextSize(i18n::tr(fm.c_str())).x + 18;
    if (cx + tw > mx + mw) cx = mx, cy += 26;
    ImGui::SetCursorScreenPos(ImVec2(cx, cy));
    ImGui::PushID(fm.c_str());
    const bool on = ui.family == fm;
    if (ImGui::InvisibleButton("##chip", ImVec2(tw, 22))) ui.family = on ? std::string() : fm;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + tw, cy + 22), on ? theme::mix(P.surface700, P.accent, 0.25f) : hov ? P.surface600 : P.surface700, 5);
    if (on) dl->AddRect(ImVec2(cx, cy), ImVec2(cx + tw, cy + 22), P.accent, 5);
    dl->AddText(ImVec2(cx + 9, cy + 4), on ? P.accent : P.textDim, i18n::tr(fm.c_str()));
    cx += tw + 6;
  }
  ImGui::PopFont();
  const float listY = cy + 30, listH = ly + lh - 8 - listY;
  std::vector<const Row*> shown;
  for (const auto& r : rows) {
    const std::string fam = patchFamily(r.bank, r.program);
    if (!ui.family.empty() && fam != ui.family) continue;
    if (!want.empty()) {
      char num[16];
      std::snprintf(num, sizeof num, "%03d:%03d", r.bank, r.program);
      const std::string hay = lower(r.name) + " " + num + " " + std::to_string(r.program) + " " + lower(fam);
      if (hay.find(want) == std::string::npos) continue;
    }
    shown.push_back(&r);
  }
  ImGui::SetCursorScreenPos(ImVec2(mx, listY));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5);
  ImGui::BeginChild("##pplist", ImVec2(mw, listH), ImGuiChildFlags_Borders);
  ImDrawList* ld = ImGui::GetWindowDrawList();
  int curRow = -1;
  for (size_t i = 0; i < shown.size(); ++i)
    if (shown[i]->path == ref.path && shown[i]->bank == ref.bank && shown[i]->program == ref.program) curRow = static_cast<int>(i);
  int pick = ui.devPick >= 0 && ui.devPick < static_cast<int>(shown.size()) ? ui.devPick : -1;
  ui.devPick = -1;
  // Up and down step through the list (as in the browser); Enter keeps the patch.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !shown.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) pick = std::min(static_cast<int>(shown.size()) - 1, curRow + 1), ui.scrollToCurrent = true;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) pick = std::max(0, curRow - 1), ui.scrollToCurrent = true;
  }
  if (shown.empty()) {
    ImGui::SetCursorPos(ImVec2(14, 12));
    ImGui::TextDisabled("%s", !f && ui.place == 0 ? i18n::tr("The file is not open (missing or still loading)")
                              : ui.place == 1 ? i18n::tr("No starred patches yet: click a star in the list")
                                              : i18n::tr("No patch matches"));
  }
  const float rowH = 28;
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(shown.size()), rowH);
  if (ui.scrollToCurrent && curRow >= 0) clip.IncludeItemByIndex(curRow);
  while (clip.Step()) {
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      const Row& r = *shown[static_cast<size_t>(i)];
      ImGui::PushID(i);
      const ImVec2 rp = ImGui::GetCursorScreenPos();
      const float rw = ImGui::GetContentRegionAvail().x;
      const bool now = i == curRow;
      const std::string key = listKey(r.path, r.bank, r.program);
      const bool star = isStarred(a, key);
      // Star toggle on the right.
      ImGui::SetCursorScreenPos(ImVec2(rp.x + rw - 26, rp.y));
      const bool starHit = ImGui::InvisibleButton("##star", ImVec2(24, rowH));
      const bool starHov = ImGui::IsItemHovered();
      ImGui::SetCursorScreenPos(rp);
      const bool hit = ImGui::InvisibleButton("##row", ImVec2(rw - 28, rowH));
      const bool hov = ImGui::IsItemHovered();
      if (starHit) toggleStar(a, key);
      if (hit) pick = i;
      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus(nullptr);
      if (now) ld->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rowH), theme::mix(P.surface800, P.accent, 0.28f));
      else if (hov || starHov) ld->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rowH), P.surface800);
      ld->AddLine(ImVec2(rp.x, rp.y + rowH - 0.5f), ImVec2(rp.x + rw, rp.y + rowH - 0.5f), theme::mix(P.surface950, P.text, 0.05f));
      char num[16];
      std::snprintf(num, sizeof num, "%03d:%03d", r.bank, r.program);
      ImGui::PushFont(app().fontMono, 11.5f);
      ld->AddText(ImVec2(rp.x + 10, rp.y + 7), now ? P.accent : P.textFaint, num);
      ImGui::PopFont();
      ImGui::PushFont(now ? app().fontBold : nullptr, 12.5f);
      ld->AddText(ImVec2(rp.x + 76, rp.y + 6), P.text, r.name.c_str());
      ImGui::PopFont();
      ImGui::PushFont(nullptr, 11.5f);
      const char* fam = i18n::tr(patchFamily(r.bank, r.program));
      ld->AddText(ImVec2(rp.x + rw - 186, rp.y + 7), P.textDim, fam);
      char zt[32];
      std::snprintf(zt, sizeof zt, r.zones == 1 ? i18n::tr("%d zone") : i18n::tr("%d zones"), r.zones);
      const ImVec2 zs = ImGui::CalcTextSize(zt);
      ld->AddText(ImVec2(rp.x + rw - 34 - zs.x, rp.y + 7), P.textFaint, zt);
      ImGui::PopFont();
      ui::drawIcon(ld, ui::Icon::Star, ImVec2(rp.x + rw - 14, rp.y + rowH * 0.5f), 12, star ? P.accent : starHov ? P.text : P.textFaint);
      if (now && ui.scrollToCurrent) {
        ImGui::SetScrollHereY(0.4f);
        ui.scrollToCurrent = false;
      }
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH));
      ImGui::Dummy(ImVec2(rw, 0));
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (pick >= 0 && pick < static_cast<int>(shown.size())) {
    const Row& r = *shown[static_cast<size_t>(pick)];
    choosePatch(a, t, r.path, r.bank, r.program, r.name);
    previewNote(a, t, ui, r.bank >= 120 ? 38 : 60);
  }
  // ---- right: what the patch is made of
  const float rx = mx + mw + 14, rw2 = o.x + kW - 14 - rx;
  smallCaps(dl, ImVec2(rx, ly + 8), i18n::tr("PATCH"));
  ImGui::PushFont(app().fontBold, 15.0f);
  dl->AddText(ImVec2(rx, ly + 24), P.text, cur >= 0 && f ? f->presets[static_cast<size_t>(cur)].name.c_str() : (ref.name.empty() ? "-" : ref.name.c_str()));
  ImGui::PopFont();
  ImGui::PushFont(nullptr, 11.5f);
  char sub[96];
  std::snprintf(sub, sizeof sub, i18n::tr("Bank %03d \xC2\xB7 Program %03d \xC2\xB7 %s"), ref.bank, ref.program, i18n::tr(patchFamily(ref.bank, ref.program)));
  dl->AddText(ImVec2(rx, ly + 46), P.textDim, sub);
  if (f && cur >= 0) {
    static std::map<std::pair<const Sf2File*, int>, PatchInfo> infos;
    auto it = infos.find({f, cur});
    if (it == infos.end()) it = infos.emplace(std::make_pair(f, cur), patchInfo(*f, static_cast<size_t>(cur))).first;
    const PatchInfo& pi = it->second;
    float yy = ly + 74;
    auto stat = [&](const char* k, const std::string& v) {
      dl->AddText(ImVec2(rx, yy), P.textDim, k);
      const ImVec2 vs = ImGui::CalcTextSize(v.c_str());
      dl->AddText(ImVec2(rx + rw2 - vs.x, yy), P.text, v.c_str());
      yy += 21;
    };
    stat(i18n::tr("Zones"), std::to_string(pi.zones));
    stat(i18n::tr("Samples"), std::to_string(pi.samples) + (pi.stereoPairs ? " (" + std::string(i18n::tr("stereo pairs")) + " " + std::to_string(pi.stereoPairs) + ")" : ""));
    stat(i18n::tr("Keys"), pi.zones ? noteName(pi.keyLo) + " " + i18n::tr("to") + " " + noteName(pi.keyHi) : "-");
    stat(i18n::tr("Velocity layers"), std::to_string(pi.velocityLayers));
    stat(i18n::tr("Loops"), pi.allLoop ? i18n::tr("all zones") : pi.loops ? i18n::tr("some zones") : i18n::tr("none"));
    stat(i18n::tr("Memory"), bytesText(pi.memory));
    stat(i18n::tr("Its own reverb"), std::to_string(static_cast<int>(std::lround(pi.reverb * 100))) + " %");
    stat(i18n::tr("Its own chorus"), std::to_string(static_cast<int>(std::lround(pi.chorus * 100))) + " %");
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(rx, ly + 262));
  ImGui::PushTextWrapPos(rx + rw2 - ImGui::GetWindowPos().x);  // (window-local)
  ImGui::PushFont(nullptr, 11.5f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     i18n::tr("Click a row to play it on this channel. Up and down step through the list, as in the browser."));
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
}

// ------------------------------------------------------------------ Sound tab
void envelopeGraph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const PluginState& st, const PatchInfo* pi, const Sf2File* f, int cur) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface950, 4);
  (void)pi;
  // The patch's own envelope (first zone) and the one the player uses.
  double a = 0.01, d = 1, s = 1, r = 0.3;
  if (f && cur >= 0) {
    auto regs = sf2PresetRegions(*f, static_cast<size_t>(cur));
    if (!regs.empty()) {
      const auto& g = regs[0].gen;
      a = std::pow(2.0, std::clamp(g[kAttackVolEnv], -12000, 8000) / 1200.0);
      d = std::pow(2.0, std::clamp(g[kDecayVolEnv], -12000, 8000) / 1200.0);
      s = std::pow(10.0, -std::clamp(g[kSustainVolEnv], 0, 1440) / 200.0);
      r = std::pow(2.0, std::clamp(g[kReleaseVolEnv], -12000, 8000) / 1200.0);
    }
  }
  auto prm = [&](int k) { return static_cast<size_t>(k) < st.params.size() ? static_cast<double>(st.params[static_cast<size_t>(k)]) : 0.0; };
  double A = a, D = d, S = s, R = r;
  const int mode = static_cast<int>(std::lround(prm(PpEnvMode)));
  if (mode == PpEnvScale) A *= prm(PpAttackScale), D *= prm(PpDecayScale), S = std::min(1.0, S * prm(PpSustainScale)), R *= prm(PpReleaseScale);
  if (mode == PpEnvReplace) A = prm(PpAttack), D = prm(PpDecay), S = prm(PpSustain), R = prm(PpRelease);
  auto curve = [&](double at, double dt, double sl, double rt, ImU32 col, bool dashed) {
    const double total = std::max(0.5, (at + dt * 0.4 + 0.6 + rt * 0.5));
    const float w = p1.x - p0.x - 16, h = p1.y - p0.y - 16;
    auto X = [&](double tt) { return p0.x + 8 + static_cast<float>(std::min(1.0, tt / total)) * w; };
    auto Y = [&](double lv) { return p1.y - 8 - static_cast<float>(lv) * h; };
    // The decay reaches the sustain level after the share of the set time its drop takes (100 dB per set time).
    const double dropDb = sl > 1e-5 ? -20 * std::log10(sl) : 100.0;
    const double dEnd = at + dt * std::min(1.0, dropDb / 100.0);
    const ImVec2 pts[] = {ImVec2(X(0), Y(0)), ImVec2(X(at), Y(1)), ImVec2(X(dEnd), Y(sl)), ImVec2(X(dEnd + 0.6), Y(sl)), ImVec2(X(dEnd + 0.6 + rt * 0.5), Y(0))};
    if (!dashed) {
      dl->AddPolyline(pts, 5, col, 0, 2.0f);
      return;
    }
    for (int i = 0; i < 4; ++i) {
      const ImVec2 q0 = pts[i], q1 = pts[i + 1];
      const float len = std::hypot(q1.x - q0.x, q1.y - q0.y);
      for (float u = 0; u < len; u += 8) {
        const float u1 = std::min(len, u + 4);
        dl->AddLine(ImVec2(q0.x + (q1.x - q0.x) * u / len, q0.y + (q1.y - q0.y) * u / len),
                    ImVec2(q0.x + (q1.x - q0.x) * u1 / len, q0.y + (q1.y - q0.y) * u1 / len), col, 1.2f);
      }
    }
  };
  if (mode != PpEnvPatch) curve(a, d, s, r, theme::withAlpha(P.text, 0.45f), true);
  curve(A, D, S, R, P.accent, false);
  if (mode != PpEnvPatch) {
    ImGui::PushFont(nullptr, 11.0f);
    const char* cap = i18n::tr("dashed: from the patch");
    const ImVec2 cs = ImGui::CalcTextSize(cap);
    dl->AddText(ImVec2(p1.x - cs.x - 8, p1.y - cs.y - 4), P.textFaint, cap);
    ImGui::PopFont();
  }
}

void soundTab(App& a, const PluginTarget& t, const PluginState& st, const Sf2File* f, int cur, ImVec2 o) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float y0 = o.y + 4, rowH = 108;
  auto K = [&](int param, float x, float y, const char* label) { pui::knobAt(a, t, param, ImVec2(x, y), label, pui::KnobSmall, 64); };
  // Row 1: output, pitch, voices, filter.
  float x = o.x + 12;
  panel(dl, ImVec2(x, y0), ImVec2(x + 290, y0 + rowH), i18n::tr("OUTPUT"));
  // The sends: two outputs of the channel; one click routes them to a reverb and a chorus track.
  if (t.isChannel()) {
    const Song& s = a.doc.song();
    const auto [rv, ch] = patchSendTracks(s, t.owner);
    const int ri = rv == kNoId ? -1 : s.mixerIndex(rv), ci = ch == kNoId ? -1 : s.mixerIndex(ch);
    ImGui::PushFont(nullptr, 11.0f);
    if (ri < 0 && ci < 0) {
      ImGui::SetCursorScreenPos(ImVec2(x + 290 - 128, y0 + 4));
      if (ui::TextButton(i18n::tr("Sends to mixer"), false, 120,
                         i18n::tr("Make a reverb track and a chorus track (Gridline's own effects) and send the Reverb and Chorus outputs to them")))
        a.doc.editSong("send reverb and chorus to mixer tracks", [&](Song& song) { routePatchSends(song, t.owner); });
    } else {
      std::string where = std::string(i18n::tr("to")) + " " + (ri >= 0 ? std::to_string(ri) : std::string("-")) + ", " +
                          (ci >= 0 ? std::to_string(ci) : std::string("-"));
      const ImVec2 ws = ImGui::CalcTextSize(where.c_str());
      dl->AddText(ImVec2(x + 290 - 10 - ws.x, y0 + 8), P.textFaint, where.c_str());
      ImGui::SetCursorScreenPos(ImVec2(x + 290 - 10 - ws.x, y0 + 6));
      ImGui::InvisibleButton("##ppsendsat", ws);
      if (ImGui::IsItemHovered())
        a.setHint(std::string(i18n::tr("Reverb send to")) + " " + (ri >= 0 ? mixerTrackName(s, static_cast<size_t>(ri)) : "-") + ", " +
                  i18n::tr("chorus send to") + " " + (ci >= 0 ? mixerTrackName(s, static_cast<size_t>(ci)) : "-"));
    }
    ImGui::PopFont();
  }
  K(PpVolume, x + 8, y0 + 28, i18n::tr("Volume"));
  K(PpPan, x + 76, y0 + 28, i18n::tr("Pan"));
  K(PpReverb, x + 146, y0 + 28, i18n::tr("Reverb"));
  K(PpChorus, x + 214, y0 + 28, i18n::tr("Chorus"));
  x += 300;
  panel(dl, ImVec2(x, y0), ImVec2(x + 290, y0 + rowH), i18n::tr("PITCH"));
  K(PpTranspose, x + 8, y0 + 28, i18n::tr("Transpose"));
  K(PpFine, x + 76, y0 + 28, i18n::tr("Fine"));
  K(PpBendRange, x + 146, y0 + 28, i18n::tr("Bend range"));
  K(PpGlide, x + 214, y0 + 28, i18n::tr("Glide"));
  x += 300;
  panel(dl, ImVec2(x, y0), ImVec2(x + 196, y0 + rowH), i18n::tr("VOICES"));
  switchRow(a, t, PpMono, ImVec2(x + 186, y0 + 5), {i18n::tr("Poly"), i18n::tr("Mono")});
  K(PpPolyphony, x + 22, y0 + 28, i18n::tr("Polyphony"));
  K(PpVelocity, x + 104, y0 + 28, i18n::tr("Velocity"));
  x += 206;
  panel(dl, ImVec2(x, y0), ImVec2(o.x + kW - 12, y0 + rowH), i18n::tr("FILTER"));
  K(PpCutoff, x + 8, y0 + 28, i18n::tr("Cutoff"));
  K(PpResonance, x + 76, y0 + 28, i18n::tr("Resonance"));
  K(PpKeyTrack, x + 144, y0 + 28, i18n::tr("Key track"));
  // Row 2: volume envelope, vibrato.
  const float y1 = y0 + rowH + 12, row2H = 134;
  x = o.x + 12;
  const float envW = 604;
  panel(dl, ImVec2(x, y1), ImVec2(x + envW, y1 + row2H), i18n::tr("VOLUME ENVELOPE"));
  switchRow(a, t, PpEnvMode, ImVec2(x + envW - 10, y1 + 5), {i18n::tr("From the patch"), i18n::tr("Scale"), i18n::tr("Replace")});
  envelopeGraph(dl, ImVec2(x + 10, y1 + 34), ImVec2(x + 310, y1 + row2H - 10), st, nullptr, f, cur);
  const int mode = st.params.size() > PpEnvMode ? static_cast<int>(std::lround(st.params[PpEnvMode])) : 0;
  const int base = mode == PpEnvReplace ? PpAttack : PpAttackScale;
  const char* names[4] = {i18n::tr("Attack"), i18n::tr("Decay"), i18n::tr("Sustain"), i18n::tr("Release")};
  if (mode == PpEnvPatch) ImGui::BeginDisabled();
  for (int k = 0; k < 4; ++k) K(base + k, x + 322 + k * 68.f, y1 + 46, names[k]);
  if (mode == PpEnvPatch) ImGui::EndDisabled();
  x += envW + 10;
  panel(dl, ImVec2(x, y1), ImVec2(o.x + kW - 12, y1 + row2H), i18n::tr("VIBRATO"));
  switchRow(a, t, PpVibMode, ImVec2(o.x + kW - 22, y1 + 5), {i18n::tr("From the patch"), i18n::tr("Own")});
  const bool own = st.params.size() > PpVibMode && std::lround(st.params[PpVibMode]) == PpVibOwn;
  if (!own) ImGui::BeginDisabled();
  K(PpVibDepth, x + 12, y1 + 46, i18n::tr("Depth"));
  K(PpVibRate, x + 80, y1 + 46, i18n::tr("Rate"));
  K(PpVibDelay, x + 148, y1 + 46, i18n::tr("Delay"));
  if (!own) ImGui::EndDisabled();
  K(PpModWheel, x + 216, y1 + 46, i18n::tr("Mod wheel"));
  // Row 3: what the patch itself asks for.
  const float y2 = y1 + row2H + 12;
  panel(dl, ImVec2(o.x + 12, y2), ImVec2(o.x + kW - 12, y2 + 52), nullptr);
  ImGui::PushFont(app().fontMonoBold, 10.5f);
  dl->AddText(ImVec2(o.x + 22, y2 + 9), P.textDim, i18n::tr("FROM THE FILE"));
  ImGui::PopFont();
  ImGui::PushFont(nullptr, 11.5f);
  dl->AddText(ImVec2(o.x + 128, y2 + 9), P.textFaint, i18n::tr("what the patch itself asks for; Gridline plays all of it"));
  if (f && cur >= 0) {
    const PatchInfo pi = patchInfo(*f, static_cast<size_t>(cur));
    char filt[64], excl[16];
    if (pi.lowPass || pi.filterQcB > 0) std::snprintf(filt, sizeof filt, i18n::tr("low-pass, Q %d dB"), pi.filterQcB / 10);
    else std::snprintf(filt, sizeof filt, "%s", i18n::tr("open"));
    if (pi.exclusiveClasses) std::snprintf(excl, sizeof excl, "%d", pi.exclusiveClasses);
    else std::snprintf(excl, sizeof excl, "%s", i18n::tr("none"));
    char line[512];
    std::snprintf(line, sizeof line,
                  i18n::tr("Modulators %d default + %d own      Mod envelope %s      LFOs %d      Filter %s      Exclusive classes %s      Sample format %s%s"),
                  pi.defaultMods, pi.ownMods,
                  i18n::tr(pi.modEnvPitch && pi.modEnvFilter ? i18n::N_("to pitch, to filter")
                           : pi.modEnvPitch                  ? i18n::N_("to pitch")
                           : pi.modEnvFilter                 ? i18n::N_("to filter")
                                                             : i18n::N_("none")),
                  pi.lfos, filt, excl,
                  i18n::tr(pi.compressed ? i18n::N_("compressed") : pi.bits24 ? i18n::N_("24-bit") : i18n::N_("16-bit")),
                  pi.stereoPairs ? i18n::tr(", stereo pairs") : "");
    dl->AddText(ImVec2(o.x + 22, y2 + 29), P.text, line);
  }
  ImGui::PopFont();
}

}  // namespace

void drawPatchPlayerEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = stateOf(a, t);
  if (!sp) return;
  const PluginState st = *sp;
  Ui& ui = gUi[t.key()];
  const auto& P = pal();
  const PatchRef ref = parsePatchRef(st.data);
  bool failed = false;
  std::string err;
  auto file = ref.path.empty() ? nullptr : requestSf2(ref.path, &failed, &err);
  const Sf2File* f = file.get();
  int cur = -1;
  if (f) cur = sf2FindPreset(*f, ref.bank, ref.program);
  if (ui.previewKey >= 0 && ImGui::GetTime() > ui.previewOff) {
    if (t.isChannel()) a.engine.noteOff(t.owner, ui.previewKey);
    ui.previewKey = -1;
  }
  ImGui::PushID("pp");
  const ImVec2 o = ImGui::GetCursorScreenPos();
  (void)width;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float H = ui.tab == 1 ? kHSound : kH;
  dl->AddRectFilled(o, ImVec2(o.x + kW, o.y + H), P.surface900, 4);
  // ---- header: file, patch, tabs
  const float hy = o.y + 8;
  dl->AddRectFilled(ImVec2(o.x + 12, hy + 2), ImVec2(o.x + 34, hy + 24), theme::mix(P.surface700, P.accent, 0.35f), 4);
  ui::drawIcon(dl, ui::Icon::Piano, ImVec2(o.x + 23, hy + 13), 13, P.accent);
  ImGui::SetCursorScreenPos(ImVec2(o.x + 42, hy));
  const std::string fname = ref.path.empty() ? std::string(i18n::tr("Choose an SF2 file...")) : fs::u8path(ref.path).filename().u8string();
  ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(P.surface950));
  if (ImGui::Button((fname + "##ppfile").c_str(), ImVec2(240, 26))) ImGui::OpenPopup("##ppfiles");
  ImGui::PopStyleColor();
  ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(o.x + 42 + 228, hy + 13), 10, P.textDim);
  if (ImGui::IsItemHovered()) a.setHint(ref.path.empty() ? std::string(i18n::tr("Open a .sf2 or .sf3 file")) : ref.path);
  if (ImGui::BeginPopup("##ppfiles")) {
    if (ImGui::MenuItem(i18n::tr("Open an SF2 file..."))) openFileDialog(a, t);
    const auto files = readList(a, "patchplayer.files");
    if (!files.empty()) ImGui::Separator();
    for (const auto& p : files) {
      if (p == ref.path) continue;
      if (ImGui::MenuItem(fs::u8path(p).filename().u8string().c_str())) {
        if (auto g = loadSf2(p)) choosePatch(a, t, p, g->presets.front().bank, g->presets.front().program, g->presets.front().name);
        else a.notify(std::string(i18n::tr("Cannot read")) + " " + p, true);
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PushFont(nullptr, 12.0f);
  std::string info;
  if (f) {
    std::vector<int> banks;
    for (const auto& p : f->presets)
      if (std::find(banks.begin(), banks.end(), p.bank) == banks.end()) banks.push_back(p.bank);
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(ref.path), ec);
    info = (ec ? std::string() : bytesText(static_cast<size_t>(size)) + " \xC2\xB7 ") + std::to_string(f->presets.size()) + " " + i18n::tr("patches") + " \xC2\xB7 " +
           std::to_string(banks.size()) + " " + (banks.size() == 1 ? i18n::tr("bank") : i18n::tr("banks")) + " \xC2\xB7 " + i18n::tr("loaded");
    if (f->undecodable) info += " \xC2\xB7 " + std::to_string(f->undecodable) + " " + i18n::tr("samples could not be decoded");
  } else if (failed && !ref.path.empty()) {
    info = i18n::tr("The file is missing");
  } else if (!ref.path.empty()) {
    info = i18n::tr("loading...");
  }
  dl->AddText(ImVec2(o.x + 292, hy + 6), failed ? P.danger : P.textDim, info.c_str());
  ImGui::PopFont();
  if (failed && !ref.path.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(o.x + 292 + 118, hy + std::round((26 - ImGui::GetFrameHeight()) * 0.5f)));
    if (ui::TextButton(i18n::tr("Find the file..."), false, 0, i18n::tr("Search for the missing file, as for samples"))) exportui::openMissingSamples(a);
  }
  // Patch field: arrows step through the file's patches; a click on the name lists them all.
  const float px = o.x + 580;
  auto step = [&](int d) {
    if (!f || f->presets.empty()) return;
    int i = cur < 0 ? 0 : std::clamp(cur + d, 0, static_cast<int>(f->presets.size()) - 1);
    const auto& pr = f->presets[static_cast<size_t>(i)];
    choosePatch(a, t, ref.path, pr.bank, pr.program, pr.name);
  };
  ImGui::SetCursorScreenPos(ImVec2(px, hy));
  if (ui::IconButton("##ppprev", ui::Icon::ChevronLeft, false, 26.0f, i18n::tr("Previous patch"))) step(-1);
  ImGui::SetCursorScreenPos(ImVec2(px + 30, hy));
  ImGui::InvisibleButton("##ppfield", ImVec2(230, 26));
  const bool fieldHov = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) ImGui::OpenPopup("##pppatches");
  dl->AddRectFilled(ImVec2(px + 30, hy), ImVec2(px + 260, hy + 26), P.surface950, 4);
  dl->AddRect(ImVec2(px + 30, hy), ImVec2(px + 260, hy + 26), fieldHov ? P.text : P.accent, 4);
  {
    char num[16];
    std::snprintf(num, sizeof num, "%03d:%03d", ref.bank, ref.program);
    ImGui::PushFont(app().fontMono, 11.5f);
    dl->AddText(ImVec2(px + 38, hy + 6), P.accent, num);
    ImGui::PopFont();
    ImGui::PushFont(app().fontBold, 12.5f);
    const std::string nm = cur >= 0 && f ? f->presets[static_cast<size_t>(cur)].name : ref.name;
    dl->AddText(ImVec2(px + 96, hy + 5), P.text, nm.c_str());
    ImGui::PopFont();
  }
  if (fieldHov) a.setHint(i18n::tr("Click: every patch of the file in a searchable list"));
  ImGui::SetCursorScreenPos(ImVec2(px + 264, hy));
  if (ui::IconButton("##ppnext", ui::Icon::Chevron, false, 26.0f, i18n::tr("Next patch"))) step(1);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(460, 380));
  if (ImGui::BeginPopup("##pppatches")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pppfind", i18n::tr("name, number or family"), ui.find, sizeof ui.find);
    const std::string want = lower(ui.find);
    ImGui::BeginChild("##ppprows", ImVec2(0, 320));
    if (f)
      for (size_t i = 0; i < f->presets.size(); ++i) {
        const auto& pr = f->presets[i];
        char row[128];
        std::snprintf(row, sizeof row, "%03d:%03d  %s##ppp%zu", pr.bank, pr.program, pr.name.c_str(), i);
        if (!want.empty() && (lower(row) + " " + lower(patchFamily(pr.bank, pr.program))).find(want) == std::string::npos) continue;
        const bool now = static_cast<int>(i) == cur;
        if (ImGui::Selectable(row, now)) {
          choosePatch(a, t, ref.path, pr.bank, pr.program, pr.name);
          ImGui::CloseCurrentPopup();
        }
        if (now && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.4f);
      }
    ImGui::EndChild();
    ImGui::EndPopup();
  }
  // Tabs.
  // The header's controls are 26 px tall: the tabs (one frame tall) sit on their centre line.
  const float tabY = hy + std::round((26 - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(o.x + kW - 142, tabY));
  if (ui::TextButton(i18n::tr("Patches"), ui.tab == 0, 64)) ui.tab = 0;
  ImGui::SameLine(0, 2);
  if (ui::TextButton(i18n::tr("Sound"), ui.tab == 1, 64)) ui.tab = 1;
  dl->AddLine(ImVec2(o.x, hy + 34), ImVec2(o.x + kW, hy + 34), theme::mix(P.surface900, P.text, 0.08f));
  // ---- body
  const ImVec2 body(o.x, hy + 40);
  if (ui.tab == 0) patchesTab(a, t, ui, ref, f, cur, body);
  else soundTab(a, t, st, f, cur, body);
  // ---- keyboard
  PatchInfo pi;
  const bool havePi = f && cur >= 0;
  if (havePi) pi = patchInfo(*f, static_cast<size_t>(cur));
  const float ky = o.y + H - 84;
  keyboard(a, t, ui, havePi ? &pi : nullptr, ImVec2(o.x + 12, ky), kW - 24, 58);
  ImGui::PushFont(nullptr, 11.0f);
  dl->AddText(ImVec2(o.x + 12, ky + 62), P.textFaint,
              ui.tab == 0 ? i18n::tr("Zones of the patch on the keys: each colour one sample range. Play the keys to hear it.")
                          : i18n::tr("Every knob is automatable and has a MIDI learn. Patch and Bank are parameters too (program change)."));
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(kW, H));
  ImGui::PopID();
}

std::string patchPlayerTitle(const PluginState& st) {
  const PatchRef r = parsePatchRef(st.data);
  if (r.path.empty()) return "";
  char num[16];
  std::snprintf(num, sizeof num, "%03d:%03d", r.bank, r.program);
  std::string name = r.name;
  if (auto f = requestSf2(r.path)) {
    const int i = sf2FindPreset(*f, r.bank, r.program);
    if (i >= 0) name = f->presets[static_cast<size_t>(i)].name;
  }
  return std::string(num) + " " + name;
}

// Dev input: "patchplayer tab <0|1>", "patchplayer find <text>", "patchplayer pick <row>",
// "patchplayer family <name|->", "patchplayer place <0..2>", "patchplayer state".
bool patchPlayerDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd != "patchplayer") return false;
  if (args.empty()) return true;
  for (auto& [k, ui] : gUi) {
    (void)k;
    if (args[0] == "tab" && args.size() > 1) ui.tab = std::atoi(args[1].c_str());
    else if (args[0] == "find") {
      std::string s;
      for (size_t i = 1; i < args.size(); ++i) s += (i > 1 ? " " : "") + args[i];
      std::snprintf(ui.find, sizeof ui.find, "%s", s.c_str());
    } else if (args[0] == "pick" && args.size() > 1) {
      ui.devPick = std::atoi(args[1].c_str());
    } else if (args[0] == "family" && args.size() > 1) {
      ui.family = args[1] == "-" ? std::string() : args[1];
    } else if (args[0] == "place" && args.size() > 1) {
      ui.place = std::atoi(args[1].c_str());
    }
  }
  if (args[0] == "open") {  // the first Patch Player channel's window (in the Tracker, as "Open plugin window")
    for (const auto& c : a.doc.song().channels)
      if (c.plugin.type == kPatchPlayerType) {
        if (layoutui::active() == layout::Preset::Tracker) trackereditors::openPluginWindow(a, c.id);
        else a.settingsChannel = c.id;
        break;
      }
  }
  if (args[0] == "sends") {  // the Sends to mixer button of the first Patch Player channel
    for (const auto& c : a.doc.song().channels)
      if (c.plugin.type == kPatchPlayerType) {
        const ObjectId id = c.id;
        a.doc.editSong("send reverb and chorus to mixer tracks", [&](Song& song) { routePatchSends(song, id); });
        break;
      }
  }
  if (args[0] == "state") {
    for (const auto& c : a.doc.song().channels)
      if (c.plugin.type == kPatchPlayerType) {
        const PatchRef r = parsePatchRef(c.plugin.data);
        const auto [rv, ch] = patchSendTracks(a.doc.song(), c.id);
        const MixerTrack* rt = a.doc.song().mixerTrack(rv);
        const MixerTrack* ct = a.doc.song().mixerTrack(ch);
        std::printf("PATCHPLAYER channel=%s bank=%d program=%d name=%s reverb=%.2f chorus=%.2f sends=%s,%s undo=\"%s\" file=%s\n", c.name.c_str(), r.bank,
                    r.program, r.name.c_str(), c.plugin.params.size() > PpChorus ? static_cast<double>(c.plugin.params[PpReverb]) : 0.0,
                    c.plugin.params.size() > PpChorus ? static_cast<double>(c.plugin.params[PpChorus]) : 0.0,
                    rt ? rt->slots[0].plugin.type.c_str() : "-", ct ? ct->slots[0].plugin.type.c_str() : "-", a.doc.history().undoLabel().c_str(),
                    r.path.c_str());
      }
    std::fflush(stdout);
  }
  return true;
}

}  // namespace gl::packs::samplers

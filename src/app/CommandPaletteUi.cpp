// Gridline - K-93 command palette (see CommandPaletteUi.h).
#include "CommandPaletteUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Keymap.h"
#include "../core/MenuStyle.h"
#include "../core/PluginInfo.h"
#include "App.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "SettingsPages.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::cmdpalette {

namespace {

using cmdpal::Entry;
using cmdpal::Kind;
using theme::pal;

int gSkip = 0;  // SkipRows depth

constexpr const char* kSep = " > ";
constexpr int kCategories = 5;  // All, then one per Kind

// --------------------------------------------------------------- unseen ----
// Draws `f` into a window nobody sees: fully clipped (nothing reaches the screen), no
// inputs, no saved settings, inside the main viewport (never a window of its own).
template <class F>
void unseen(const char* id, F&& f) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(ImVec2(820, 600));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::SetNextWindowBgAlpha(0.0f);
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
  ImGui::Begin(id, nullptr, fl);
  ImGui::PushClipRect(ImVec2(-8, -8), ImVec2(-8, -8), false);
  f();
  ImGui::PopClipRect();
  ImGui::End();
}

std::string join(const std::vector<std::string>& v, const char* sep, size_t from = 0) {
  std::string out;
  for (size_t i = from; i < v.size(); ++i) {
    if (i > from) out += sep;
    out += v[i];
  }
  return out;
}

// ---------------------------------------------------------- menu walk ----
struct MenuRec {
  int root = 0;
  std::vector<std::string> raw;        // submenu labels as the menu code passes them (the root excluded)
  std::vector<std::string> shown, en;  // the path as shown and in English (root included)
  std::string label;                   // as passed
  std::string labelShown, labelEn;
  std::string shortcut;
  const char* cmd = nullptr;
  bool selected = false, enabled = true;
  std::string hint;
  int occ = 0;
  std::string rowId;  // the palette row it became
};

// The keymap command behind a menu item's shortcut text.
const char* commandOf(const char* shortcut) {
  if (!shortcut || !*shortcut) return nullptr;
  if (const char* c = keys::commandForLabel(shortcut)) return c;
  const keys::Chord ch = keys::parse(shortcut);
  if (!ch.valid()) return nullptr;
  const auto v = keys::keymap().lookup(keys::Context::Global, ch);
  return v.size() == 1 ? v[0]->id : nullptr;
}

struct ListWalker : imhook::MenuWalker {
  struct Level {
    std::string raw, shown, en;
    bool enabled = true;
  };
  std::vector<MenuRec>* out = nullptr;
  int root = 0;
  std::vector<Level> stack;  // [0] is the root menu
  std::map<std::string, int> seen;
  bool lastWasItem = false;
  ImGuiID lastItem = 0;

  bool beginMenu(const char* label, bool enabled) override {
    const char* en = i18n::source(label);
    stack.push_back({label, cmdpal::stripId(label), cmdpal::stripId(en ? en : label), enabled && stack.back().enabled});
    lastWasItem = false;
    return true;
  }
  void endMenu() override {
    if (stack.size() > 1) stack.pop_back();
    lastWasItem = false;
  }
  bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled) override {
    lastWasItem = false;
    if (gSkip > 0 || !label) return false;
    MenuRec r;
    r.root = root;
    for (size_t i = 0; i < stack.size(); ++i) {
      if (i > 0) r.raw.push_back(stack[i].raw);
      r.shown.push_back(stack[i].shown);
      r.en.push_back(stack[i].en);
    }
    r.label = label;
    r.labelShown = cmdpal::stripId(label);
    const char* en = i18n::source(label);
    r.labelEn = cmdpal::stripId(en ? en : label);
    if (r.labelShown.empty()) return false;
    r.shortcut = shortcut ? shortcut : "";
    r.cmd = commandOf(shortcut);
    r.selected = selected;
    r.enabled = enabled && stack.back().enabled;
    const std::string key = std::to_string(root) + "\x1f" + join(r.raw, "\x1f") + "\x1f" + label;
    r.occ = seen[key]++;
    out->push_back(std::move(r));
    lastWasItem = true;
    lastItem = ImGui::GetItemID();
    return false;
  }
};

struct RunWalker : imhook::MenuWalker {
  std::vector<std::string> raw;
  std::string label;
  int occ = 0, seen = 0;
  size_t depth = 0;
  bool done = false, blocked = false;
  bool beginMenu(const char* l, bool) override {
    if (done || depth >= raw.size() || raw[depth] != l) return false;
    ++depth;
    return true;
  }
  void endMenu() override {
    if (depth > 0) --depth;
  }
  bool menuItem(const char* l, const char*, bool, bool enabled) override {
    if (done || gSkip > 0 || !l || depth != raw.size() || label != l) return false;
    if (seen++ != occ) return false;
    done = true;
    if (!enabled) {
      blocked = true;
      return false;
    }
    return true;
  }
};

ListWalker* gListing = nullptr;  // the walk noteHint() belongs to

const char* rootName(const menustyle::Menu& m) { return m.name ? m.name : "Gridline"; }

void walkMenus(App& a, std::vector<MenuRec>& out) {
  const auto& menus = menustyle::menus(layoutui::menuStyle(a));
  unseen("##cmdpalette_menus", [&] {
    for (size_t i = 0; i < menus.size(); ++i) {
      ListWalker w;
      w.out = &out;
      w.root = static_cast<int>(i);
      const char* name = rootName(menus[i]);
      w.stack.push_back({name, cmdpal::stripId(i18n::tr(name)), name, true});
      gListing = &w;
      detail::noting = true;
      imhook::setMenuWalker(&w);
      drawStyleMenu(a, menus[i]);
      imhook::setMenuWalker(nullptr);
      gListing = nullptr;
    }
  });
}

// --------------------------------------------------------- settings walk ----
struct SetRec {
  int page = 0;
  std::string label, labelEn, sectionEn;
};
struct SettingsWalk {
  bool on = false;
  int page = 0;
  std::string section;
  std::vector<SetRec>* out = nullptr;
  std::set<std::string> seen;
};
SettingsWalk gSet;

void addSetting(const char* shown) {
  if (!shown || !*shown || !gSet.out) return;
  SetRec r;
  r.page = gSet.page;
  r.label = cmdpal::stripId(shown);
  const char* en = i18n::source(shown);
  r.labelEn = cmdpal::stripId(en ? en : shown);
  // Trim the trailing colon some form labels carry.
  while (!r.label.empty() && (r.label.back() == ' ' || r.label.back() == ':')) r.label.pop_back();
  while (!r.labelEn.empty() && (r.labelEn.back() == ' ' || r.labelEn.back() == ':')) r.labelEn.pop_back();
  if (r.label.empty()) return;
  r.sectionEn = gSet.section;
  if (!gSet.seen.insert(std::to_string(r.page) + "/" + r.labelEn).second) return;
  gSet.out->push_back(std::move(r));
}

// The settings row the palette opened, lit for a moment in the settings window.
struct Highlight {
  std::string label;  // as shown (no "##")
  int page = -1;
  double until = 0;
  bool scrolled = false;
};
Highlight gHi;

bool inSettingsWindow() {
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  return w && w->RootWindow && std::strcmp(ui::stableWindowName(w->RootWindow->Name), "Settings") == 0;
}

void lightRow(ImVec2 a, ImVec2 b) {
  const double left = gHi.until - ImGui::GetTime();
  const float k = static_cast<float>(std::clamp(left / 0.8, 0.0, 1.0));
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, theme::withAlpha(P.accent, 0.16f * k), theme::kRadiusLg);
  dl->AddRect(a, b, theme::withAlpha(P.accent, 0.75f * k), theme::kRadiusLg, 0, 1.5f);
  if (!gHi.scrolled) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::SetScrollFromPosY(a.y - w->Pos.y, 0.35f);
    gHi.scrolled = true;
  }
}

void infoHook(ImGuiID, const char* label, int flags) {
  if (!label || !(flags & ImGuiItemStatusFlags_Checkable)) return;
  if (gSet.on) {
    addSetting(label);
    return;
  }
  if (!gHi.label.empty() && ImGui::GetTime() < gHi.until && cmdpal::stripId(label) == gHi.label && inSettingsWindow()) {
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    lightRow(ImVec2(mn.x - 6, mn.y - 3), ImVec2(mx.x + 6, mx.y + 3));
  }
}

void walkSettings(App& a, std::vector<SetRec>& out) {
  gSet = SettingsWalk{};
  gSet.on = true;
  gSet.out = &out;
  detail::noting = true;
  imhook::setInfoHook(infoHook);
  unseen("##cmdpalette_settings", [&] {
    for (int p = 0; p < settingsPageCount(); ++p) {
      if (std::strcmp(settingsPageNameEn(p), "Shortcuts") == 0) continue;  // its rows are the keymap, listed as commands
      gSet.page = p;
      gSet.section.clear();
      ImGui::PushID(p);
      drawSettingsPage(a, p, 320.0f);
      ImGui::PopID();
    }
  });
  gSet.on = false;
  gSet.out = nullptr;
  imhook::setInfoHook(nullptr);
}

// ------------------------------------------------------------- actions ----
enum class Act { Menu, Key, Window, Picker, Channel, Effect, Setting, Plugin };
struct Action {
  Act act = Act::Menu;
  int root = 0;  // Menu
  std::vector<std::string> raw;
  std::string label;
  int occ = 0;
  std::string cmd;  // Key
  keys::Context ctx = keys::Context::Global;
  std::string window;  // Window: stable name, and its flag
  bool App::*flag = nullptr;
  ObjectId obj = kNoId;  // Channel / Effect
  int slot = -1;
  int page = 0;  // Setting
  std::string type;  // Plugin
};

struct WinDef {
  const char* name;    // stable window name
  const char* menuEn;  // the menu item that opens it (English)
  bool App::*flag;     // nullptr: the plugin picker
  const char* cmd;     // keymap command that shows it, or nullptr
};
const WinDef kWindows[] = {
    {"Playlist", "Playlist", &App::showPlaylist, "view.playlist"},
    {"Channel rack", "Channel rack", &App::showRack, "view.channelRack"},
    {"Piano roll", "Piano roll", &App::showPiano, "view.pianoRoll"},
    {"Mixer", "Mixer", &App::showMixer, "view.mixer"},
    {"Browser", "Browser", &App::showBrowser, "view.browser"},
    {"Plugin picker", "Plugin picker", nullptr, "view.pluginPicker"},
    {"Master chain", "Master chain", &App::showMaster, nullptr},
    {"Live pads", "Live pads", &App::showLivePads, nullptr},
    {"Undo history", "Undo history", &App::showHistory, "edit.history"},
    {"Performance", "Performance", &App::showPerformance, nullptr},
    {"Groove editor", "Groove editor", &App::showGroove, nullptr},  // K-151
    {"Settings", "Settings...", &App::showSettings, nullptr},
    {"Plugin manager", "Plugin manager...", &App::showPluginManager, nullptr},
    {"Keyboard shortcuts", "Keyboard shortcuts", &App::showShortcuts, "help.shortcuts"},
    {"Project info", "Project info...", &App::showInfo, "file.projectInfo"},
    {"About Gridline", "About Gridline", &App::showAbout, nullptr},
};
constexpr int kWindowCount = static_cast<int>(sizeof kWindows / sizeof kWindows[0]);

// Where a keymap command without a menu item lives.
const char* commandHomeEn(const keys::Command& c) {
  if (c.context != keys::Context::Global) return keys::contextName(c.context);
  const std::string id = c.id;
  static const std::pair<const char*, const char*> kHomes[] = {
      {"file.", i18n::N_("File")},       {"edit.", i18n::N_("Edit")},       {"transport.", i18n::N_("Transport")},
      {"pattern.", i18n::N_("Patterns")}, {"view.", i18n::N_("View")},       {"options.", i18n::N_("Options")},
      {"tools.", i18n::N_("Tools")},     {"help.", i18n::N_("Help")},
  };
  for (const auto& [prefix, name] : kHomes)
    if (id.compare(0, std::strlen(prefix), prefix) == 0) return name;
  return keys::contextName(c.context);
}

const char* contextWindow(keys::Context c) {
  switch (c) {
    case keys::Context::Playlist: return "Playlist";
    case keys::Context::PianoRoll: return "Piano roll";
    case keys::Context::ChannelRack: return "Channel rack";
    case keys::Context::Mixer: return "Mixer";
    case keys::Context::Automation: return "autoedit";
    default: return nullptr;
  }
}

bool App::*contextFlag(keys::Context c) {
  switch (c) {
    case keys::Context::Playlist: return &App::showPlaylist;
    case keys::Context::PianoRoll: return &App::showPiano;
    case keys::Context::ChannelRack: return &App::showRack;
    case keys::Context::Mixer: return &App::showMixer;
    default: return nullptr;
  }
}

// ------------------------------------------------------------- state ----
struct State {
  bool open = false, wantFocus = false, needBuild = false, built = false;
  int openFrame = -10;
  char query[192] = "";
  int cat = 0;
  int sel = 0;
  bool scrollToSel = false;
  std::string rankedQuery = "\x01";
  int rankedCat = -1;
  std::vector<Entry> rows;
  std::vector<Action> acts;
  std::vector<MenuRec> menuRecs;  // for the dev dump
  std::vector<cmdpal::Ranked> ranked;
  std::vector<std::string> recent;
  int counts[kCategories] = {};
  std::string prevFocus;  // the window to give the focus back to
  // A row chosen: run at the start of the next draw (the palette is gone by then).
  bool pending = false;
  Action pendingAct;
  // A keymap command waiting for its editor's focus.
  std::string injectCmd;
  keys::Context injectCtx = keys::Context::Global;
  int injectWait = 0;
  // Dev requests (handled inside a frame).
  bool devDump = false, devState = false;
  std::string devRun;
  int devFrame = 0;
};
State S;

void addRow(std::vector<Entry>& rows, std::vector<Action>& acts, Entry e, Action act) {
  e.order = static_cast<int>(rows.size());
  rows.push_back(std::move(e));
  acts.push_back(std::move(act));
}

void build(App& a) {
  std::vector<Entry> rows;
  std::vector<Action> acts;
  std::map<std::string, size_t> at;  // row id -> index
  std::set<std::string> usedCmds;
  std::vector<int> winRow(kWindowCount, -1);
  std::map<std::string, int> sameName;  // items of one menu that read the same ("Empty project" twice: a template of that name)

  // 1. The menus, as drawn.
  std::vector<MenuRec> recs;
  walkMenus(a, recs);
  for (MenuRec& r : recs) {
    // A window's own menu item: the window row.
    int win = -1;
    for (int k = 0; k < kWindowCount && win < 0; ++k)
      if (r.labelEn == kWindows[k].menuEn || (r.cmd && kWindows[k].cmd && std::strcmp(r.cmd, kWindows[k].cmd) == 0)) win = k;
    if (win >= 0) {
      const WinDef& w = kWindows[win];
      const std::string id = std::string("win:") + w.name;
      if (winRow[static_cast<size_t>(win)] < 0) {
        Entry e;
        e.id = id;
        e.kind = Kind::Window;
        e.label = i18n::tr(w.name);
        e.labelEn = w.name;
        e.path = join(r.shown, kSep);
        e.pathEn = join(r.en, kSep);
        if (w.cmd) {
          e.commandId = w.cmd;
          if (const char* l = keys::label(w.cmd)) e.shortcut = l;
        }
        e.alias = r.labelShown;
        e.aliasEn = r.labelEn;
        e.hint = r.hint;
        Action act;
        act.act = w.flag ? Act::Window : Act::Picker;
        act.window = w.name;
        act.flag = w.flag;
        winRow[static_cast<size_t>(win)] = static_cast<int>(rows.size());
        at[id] = rows.size();
        addRow(rows, acts, std::move(e), std::move(act));
      }
      if (w.cmd) usedCmds.insert(w.cmd);
      if (r.cmd) usedCmds.insert(r.cmd);
      r.rowId = id;
      continue;
    }
    std::string id;
    if (r.cmd) {
      id = std::string("cmd:") + r.cmd;
    } else {
      id = "menu:" + join(r.en, "/") + "/" + r.labelEn;
      if (const int n = sameName[id]++; n > 0) id += "#" + std::to_string(n);
    }
    r.rowId = id;
    if (at.count(id)) continue;  // the same command in another menu: one row, where it is first found
    Entry e;
    e.id = id;
    e.kind = Kind::Command;
    e.label = r.labelShown;
    e.labelEn = r.labelEn;
    e.path = join(r.shown, kSep);
    e.pathEn = join(r.en, kSep);
    e.shortcut = r.shortcut;
    e.enabled = r.enabled;
    e.checked = r.selected;
    e.checkable = r.selected;
    e.hint = r.hint;
    if (r.cmd) {
      e.commandId = r.cmd;
      usedCmds.insert(r.cmd);
      if (const keys::Command* c = keys::findCommand(r.cmd)) {
        e.alias = i18n::tr(c->label);
        e.aliasEn = c->label;
      }
    }
    Action act;
    act.act = Act::Menu;
    act.root = r.root;
    act.raw = r.raw;
    act.label = r.label;
    act.occ = r.occ;
    at[id] = rows.size();
    addRow(rows, acts, std::move(e), std::move(act));
  }

  // 2. The windows no menu item opened.
  for (int k = 0; k < kWindowCount; ++k) {
    if (winRow[static_cast<size_t>(k)] >= 0) continue;
    const WinDef& w = kWindows[k];
    Entry e;
    e.id = std::string("win:") + w.name;
    e.kind = Kind::Window;
    e.label = i18n::tr(w.name);
    e.labelEn = w.name;
    e.path = i18n::tr("Window");
    e.pathEn = "Window";
    if (w.cmd) {
      e.commandId = w.cmd;
      usedCmds.insert(w.cmd);
      if (const char* l = keys::label(w.cmd)) e.shortcut = l;
    }
    Action act;
    act.act = w.flag ? Act::Window : Act::Picker;
    act.window = w.name;
    act.flag = w.flag;
    addRow(rows, acts, std::move(e), std::move(act));
  }
  // Each channel's instrument window and each effect's window.
  const Song& s = a.doc.song();
  for (const auto& c : s.channels) {
    const PluginInfo* p = findPlugin(c.plugin.type);
    Entry e;
    e.id = "win:chan:" + std::to_string(c.id);
    e.kind = Kind::Window;
    e.label = c.name;
    e.path = std::string(i18n::tr("Channel rack")) + kSep + (p ? p->name : c.plugin.type);
    e.pathEn = std::string("Channel rack") + kSep + (p ? p->name : c.plugin.type);
    Action act;
    act.act = Act::Channel;
    act.obj = c.id;
    addRow(rows, acts, std::move(e), std::move(act));
  }
  for (size_t t = 0; t < s.mixer.size(); ++t)
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& slot = s.mixer[t].slots[static_cast<size_t>(k)];
      if (slot.plugin.empty()) continue;
      const PluginInfo* p = findPlugin(slot.plugin.type);
      Entry e;
      e.id = "win:fx:" + std::to_string(s.mixer[t].id) + ":" + std::to_string(k);
      e.kind = Kind::Window;
      e.label = p ? p->name : slot.plugin.type;
      e.path = std::string(i18n::tr("Mixer")) + kSep + mixerTrackName(s, t);
      e.pathEn = std::string("Mixer") + kSep + mixerTrackName(s, t);
      Action act;
      act.act = Act::Effect;
      act.obj = s.mixer[t].id;
      act.slot = k;
      addRow(rows, acts, std::move(e), std::move(act));
    }

  // 3. Keymap commands no menu item runs.
  ObjectId autoClip = kNoId, autoPattern = kNoId;
  ParamId autoTarget{};
  const bool autoOpen = automationEditorTarget(&autoClip, &autoPattern, &autoTarget);
  for (const keys::Command& c : keys::commands()) {
    if (usedCmds.count(c.id)) continue;
    Entry e;
    e.id = std::string("cmd:") + c.id;
    e.kind = Kind::Command;
    e.label = i18n::tr(c.label);
    e.labelEn = c.label;
    const char* home = commandHomeEn(c);
    e.path = i18n::tr(home);
    e.pathEn = home;
    if (const char* l = keys::label(c.id)) e.shortcut = l;
    e.commandId = c.id;
    const std::string id = c.id;
    if (id == "playlist.uniqueCopy" || id == "playlist.cancelDrag" || id == "piano.cancelDrag") {
      e.enabled = false;
      e.hint = i18n::tr("Works while dragging");
    } else if (id == "rack.insertStep" || id == "rack.deleteStep") {
      e.enabled = false;
      e.hint = i18n::tr("Works with the pointer over a step");
    } else if (c.context == keys::Context::Automation && !autoOpen) {
      e.enabled = false;
      e.hint = i18n::tr("Open an automation clip first");
    }
    Action act;
    act.act = Act::Key;
    act.cmd = c.id;
    act.ctx = c.context;
    addRow(rows, acts, std::move(e), std::move(act));
  }

  // 4. Settings.
  std::vector<SetRec> sets;
  walkSettings(a, sets);
  for (const SetRec& r : sets) {
    const char* pageEn = settingsPageNameEn(r.page);
    Entry e;
    e.id = std::string("set:") + pageEn + "/" + r.labelEn;
    e.kind = Kind::Setting;
    e.label = r.label;
    e.labelEn = r.labelEn;
    e.path = std::string(i18n::tr("Settings")) + kSep + i18n::tr(pageEn);
    e.pathEn = std::string("Settings") + kSep + pageEn;
    if (!r.sectionEn.empty()) {
      e.alias = i18n::tr(r.sectionEn.c_str());
      e.aliasEn = r.sectionEn;
    }
    Action act;
    act.act = Act::Setting;
    act.page = r.page;
    act.label = r.label;
    if (at.count(e.id)) continue;
    at[e.id] = rows.size();
    addRow(rows, acts, std::move(e), std::move(act));
  }

  // 5. Plugins, as the plugin picker lists them.
  for (const PluginInfo* p : browserui::pickerPlugins(a)) {
    Entry e;
    e.id = "plug:" + p->type;
    if (at.count(e.id)) continue;
    e.kind = Kind::Plugin;
    e.label = p->name;
    const char* kindEn = p->kind == PluginKind::Generator ? i18n::N_("Instruments") : p->kind == PluginKind::Effect ? i18n::N_("Effects") : i18n::N_("Note effects");
    const std::string cat = p->category.empty() ? std::string("Other") : p->category;
    e.path = std::string(i18n::tr(kindEn)) + kSep + i18n::tr(cat.c_str());
    e.pathEn = std::string(kindEn) + kSep + cat;
    Action act;
    act.act = Act::Plugin;
    act.type = p->type;
    at[e.id] = rows.size();
    addRow(rows, acts, std::move(e), std::move(act));
  }

  S.rows = std::move(rows);
  S.acts = std::move(acts);
  S.menuRecs = std::move(recs);
  S.built = true;
  S.rankedQuery = "\x01";  // re-rank
}

// ------------------------------------------------------------- running ----
void runMenu(App& a, const Action& act) {
  const auto& menus = menustyle::menus(layoutui::menuStyle(a));
  if (act.root < 0 || act.root >= static_cast<int>(menus.size())) return;
  RunWalker w;
  w.raw = act.raw;
  w.label = act.label;
  w.occ = act.occ;
  unseen("##cmdpalette_run", [&] {
    imhook::setMenuWalker(&w);
    drawStyleMenu(a, menus[static_cast<size_t>(act.root)]);
    imhook::setMenuWalker(nullptr);
  });
  if (!w.done) a.notify(i18n::trMessage("That menu item is no longer there"), true);
  else if (w.blocked) a.notify(i18n::trMessage("That menu item cannot run right now"), true);
}

void showWindow(App& a, bool App::*flag, const char* name) {
  if (flag) a.*flag = true;
  if (name) a.focusWindow = name;
}

void perform(App& a, const Action& act) {
  switch (act.act) {
    case Act::Menu: runMenu(a, act); break;
    case Act::Key: {
      if (act.ctx == keys::Context::Global) {
        keys::inject(act.cmd.c_str(), ImGui::GetFrameCount() + 1);
        break;
      }
      showWindow(a, contextFlag(act.ctx), contextWindow(act.ctx));
      S.injectCmd = act.cmd;
      S.injectCtx = act.ctx;
      S.injectWait = 8;
      break;
    }
    case Act::Window: showWindow(a, act.flag, act.window.c_str()); break;
    case Act::Picker:
      if (!browserui::pluginPickerOpen()) browserui::togglePluginPicker();
      break;
    case Act::Channel:
      if (a.doc.song().channel(act.obj)) {
        a.selChannel = act.obj;
        a.settingsChannel = act.obj;
      }
      break;
    case Act::Effect:
      if (a.doc.song().mixerTrack(act.obj)) {
        a.selMixer = act.obj;
        a.fxEditor = {act.obj, act.slot};
      }
      break;
    case Act::Setting:
      a.showSettings = true;
      a.settingsTab = act.page;
      a.focusWindow = "Settings";
      gHi = Highlight{act.label, act.page, ImGui::GetTime() + 3.2, false};
      break;
    case Act::Plugin: browserui::pickerAdd(a, act.type); break;
  }
}

void closeNow() {
  if (!S.open) return;
  S.open = false;
  if (!S.prevFocus.empty()) ImGui::SetWindowFocus(S.prevFocus.c_str());
}

bool choose(App& a, int row) {
  if (row < 0 || row >= static_cast<int>(S.rows.size())) return false;
  const Entry& e = S.rows[static_cast<size_t>(row)];
  if (!e.enabled) return false;
  cmdpal::pushRecent(S.recent, e.id);
  a.settings.set("ui.paletteRecent", cmdpal::serializeRecent(S.recent));
  S.pending = true;
  S.pendingAct = S.acts[static_cast<size_t>(row)];
  closeNow();
  return true;
}

void tickInject() {
  if (S.injectCmd.empty()) return;
  if (keys::focusContext() == S.injectCtx || --S.injectWait <= 0) {
    keys::inject(S.injectCmd.c_str(), ImGui::GetFrameCount() + 1);
    S.injectCmd.clear();
  }
}

// ------------------------------------------------------------- ranking ----
void rerank() {
  const std::string q = S.query;
  if (q == S.rankedQuery && S.cat == S.rankedCat) return;
  S.ranked = cmdpal::rank(S.rows, q, S.recent, S.cat == 0 ? -1 : S.cat - 1);
  // Counts per category for the tabs.
  std::fill(S.counts, S.counts + kCategories, 0);
  for (const auto& r : (S.cat == 0 ? S.ranked : cmdpal::rank(S.rows, q, S.recent, -1))) {
    ++S.counts[0];
    ++S.counts[1 + static_cast<int>(S.rows[static_cast<size_t>(r.index)].kind)];
  }
  S.rankedQuery = q;
  S.rankedCat = S.cat;
  S.sel = 0;
  S.scrollToSel = true;
}

// --------------------------------------------------------------- drawing ----
ui::Icon iconFor(const Entry& e, const Action& act) {
  switch (e.kind) {
    case Kind::Window: return ui::Icon::WindowPopout;
    case Kind::Setting: return ui::Icon::Settings;
    case Kind::Plugin: return ui::Icon::Plug;
    default: return act.act == Act::Key ? ui::Icon::Keyboard : ui::Icon::ListBullets;
  }
}

// Keycaps of a shortcut, right-aligned at `right`; returns the left edge used.
float drawKeycaps(ImDrawList* dl, App& a, const std::string& shortcut, float right, float cy, bool dim) {
  if (shortcut.empty()) return right;
  const auto& P = pal();
  std::vector<std::string> parts;
  const keys::Chord c = keys::parse(shortcut);
  if (c.valid()) {
    if (c.mods & keys::kCtrl) parts.push_back("Ctrl");
    if (c.mods & keys::kCmd) parts.push_back("Cmd");
    if (c.mods & keys::kAlt) parts.push_back("Alt");
    if (c.mods & keys::kShift) parts.push_back("Shift");
    parts.push_back(keys::keyName(c.key));
  } else {
    parts.push_back(shortcut);
  }
  const float fs = theme::kFontLabel, h = 18.0f, padX = 5.0f, gap = 3.0f;
  float x = right;
  for (size_t i = parts.size(); i-- > 0;) {
    const ImVec2 ts = a.fontMono->CalcTextSizeA(fs, 1e9f, 0.0f, parts[i].c_str());
    const float w = std::max(h, std::ceil(ts.x) + 2 * padX);
    const ImVec2 p0(std::round(x - w), std::round(cy - h * 0.5f)), p1(std::round(x), std::round(cy + h * 0.5f));
    dl->AddRectFilled(p0, p1, P.field, theme::kRadius);
    dl->AddRect(p0, p1, P.line, theme::kRadius);
    dl->AddText(a.fontMono, fs, ImVec2(std::round(p0.x + (w - ts.x) * 0.5f), std::round(cy - ts.y * 0.5f)), dim ? P.textFaint : P.textDim, parts[i].c_str());
    x = p0.x - gap;
  }
  return x + gap;
}

// The label with its matched letters lit.
void drawLabel(ImDrawList* dl, ImVec2 pos, float maxW, const std::string& text, const std::vector<int>& hits, ImU32 col, ImU32 hitCol) {
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const char* shown = theme::fitText(text.c_str(), maxW);
  const std::string t = shown;
  const bool cut = t != text;
  std::vector<bool> lit(t.size(), false);
  if (!cut)
    for (int h : hits)
      if (h >= 0 && h < static_cast<int>(t.size())) lit[static_cast<size_t>(h)] = true;
  // A UTF-8 character is lit or not as a whole (its lead byte decides).
  for (size_t i = 1; i < t.size(); ++i)
    if ((static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) lit[i] = lit[i - 1];
  float x = pos.x;
  size_t i = 0;
  while (i < t.size()) {
    size_t j = i;
    while (j < t.size() && lit[j] == lit[i]) ++j;
    const char* b = t.c_str() + i;
    const char* e = t.c_str() + j;
    dl->AddText(font, fs, ImVec2(x, pos.y), lit[i] ? hitCol : col, b, e);
    x += font->CalcTextSizeA(fs, 1e9f, 0.0f, b, e).x;
    i = j;
  }
}

constexpr float kRowH = 32.0f, kHeadH = 26.0f, kPadX = 8.0f;

struct VItem {
  int ranked = -1;          // -1: a heading
  const char* head = nullptr;
  float y = 0, h = 0;
};

void drawPanel(App& a) {
  const auto& P = pal();
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  rerank();

  // Rows and headings (the empty query: Recent, then everything of the category).
  std::vector<VItem> items;
  float y = 0;
  const bool empty = S.query[0] == 0;
  bool anyRecent = false;
  for (size_t i = 0; i < S.ranked.size(); ++i) {
    const bool rec = S.ranked[i].recent;
    if (empty && i == 0 && rec) {
      items.push_back({-1, i18n::tr("RECENT"), y, kHeadH});
      y += kHeadH;
      anyRecent = true;
    }
    if (empty && anyRecent && !rec && (i == 0 || S.ranked[i - 1].recent)) {
      items.push_back({-1, S.cat == 0 ? i18n::tr("EVERYTHING") : i18n::tr(cmdpal::kindName(static_cast<Kind>(S.cat - 1))), y, kHeadH});
      y += kHeadH;
    }
    items.push_back({static_cast<int>(i), nullptr, y, kRowH});
    y += kRowH;
  }
  const float contentH = y;

  // Geometry: 640 wide (less on narrow windows), the list at most 10 rows high.
  const float W = std::floor(std::min(640.0f, vp->WorkSize.x - 2 * theme::kS4));
  const float fieldH = 38.0f, tabsH = 26.0f, footH = 34.0f;
  const float chrome = theme::kS4 + fieldH + theme::kS3 + tabsH + theme::kS2 + 1 + 1 + footH;
  const float maxList = std::max(kRowH * 3, std::min(kRowH * 10 + kHeadH * 2, vp->WorkSize.y * 0.66f - chrome));
  const float listH = std::floor(std::max(kRowH * 3, std::min(maxList, std::max(contentH, kRowH * 3))));
  const float H = chrome + listH;
  const ImVec2 pos(std::floor(vp->WorkPos.x + (vp->WorkSize.x - W) * 0.5f),
                   std::floor(vp->WorkPos.y + std::max(56.0f, std::min((vp->WorkSize.y - H) * 0.24f, vp->WorkSize.y - H - 16))));

  // The scrim: the app dims behind the palette; a click on it closes the palette.
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::SetNextWindowViewport(vp->ID);
  if (S.wantFocus) ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(IM_COL32(0, 0, 0, P.dark ? 92 : 56)));
  ImGui::Begin("##cmdpalette_scrim", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoMove);
  {
    // A soft shadow under the panel.
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    for (int k = 1; k <= 6; ++k) {
      const float g = static_cast<float>(k) * 2.0f;
      sdl->AddRectFilled(ImVec2(pos.x - g, pos.y - g + 4), ImVec2(pos.x + W + g, pos.y + H + g + 6), IM_COL32(0, 0, 0, 16 - 2 * k), 10.0f + g);
    }
    if (!S.wantFocus && ImGui::IsWindowHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) closeNow();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!S.open) return;

  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::SetNextWindowViewport(vp->ID);
  if (S.wantFocus) ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::popupBg(P)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.lineStrong));
  ImGui::Begin("##cmdpalette", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();

  // ---- search field ----
  ImGui::SetCursorScreenPos(ImVec2(wp.x + theme::kS4, wp.y + theme::kS4));
  {
    const ImVec2 fp = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(36.0f, std::floor((fieldH - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::SetNextItemWidth(W - 2 * theme::kS4);
    if (S.wantFocus || (!ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##cmdq", i18n::tr("Search commands, menus, windows, settings and plugins"), S.query, sizeof S.query);
    ImGui::PopStyleVar(2);
    ui::drawIcon(dl, ui::Icon::Search, ImVec2(fp.x + 18, fp.y + fieldH * 0.5f), 14, P.textDim);
    // "Esc" at the field's right end.
    drawKeycaps(dl, a, "Esc", fp.x + W - 2 * theme::kS4 - 10, fp.y + fieldH * 0.5f, true);
  }

  // ---- categories ----
  ImGui::SetCursorScreenPos(ImVec2(wp.x + theme::kS4, wp.y + theme::kS4 + fieldH + theme::kS3));
  {
    static std::string labelText[kCategories];
    static const char* labels[kCategories];
    const char* names[kCategories] = {i18n::N_("All"), cmdpal::kindName(Kind::Command), cmdpal::kindName(Kind::Window), cmdpal::kindName(Kind::Setting),
                                      cmdpal::kindName(Kind::Plugin)};
    for (int i = 0; i < kCategories; ++i) {
      labelText[i] = names[i];
      labels[i] = labelText[i].c_str();
    }
    // Matches in each category, right-aligned, when the categories leave room for it (long
    // translations: the categories share the whole width and the count goes).
    char buf[64];
    std::snprintf(buf, sizeof buf, i18n::tr("%d found"), S.counts[S.cat]);
    const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0.0f, buf);
    float segW = 0.0f;
    for (int i = 0; i < kCategories; ++i) segW = std::max(segW, ImGui::CalcTextSize(i18n::tr(labels[i])).x + ImGui::GetStyle().FramePadding.x * 2);
    const float avail = W - theme::kS4 * 2;
    const bool showCount = segW * kCategories + 4 + theme::kS4 + ts.x <= avail;
    if (segW * kCategories + 4 > avail) segW = std::floor((avail - 4) / kCategories);
    int cat = S.cat;
    if (ui::Segmented("##cmdcat", labels, kCategories, &cat, segW, tabsH)) S.cat = cat;
    if (showCount) {
      const float ty = wp.y + theme::kS4 + fieldH + theme::kS3 + (tabsH - ts.y) * 0.5f;
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(wp.x + W - theme::kS4 - ts.x), std::round(ty)), P.textFaint, buf);
    }
  }

  // ---- keys ----
  const int n = static_cast<int>(S.ranked.size());
  const bool typing = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (typing && !S.wantFocus) {
    const int page = std::max(1, static_cast<int>(listH / kRowH) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) S.sel = std::min(n - 1, S.sel + 1), S.scrollToSel = true;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) S.sel = std::max(0, S.sel - 1), S.scrollToSel = true;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) S.sel = std::min(n - 1, S.sel + page), S.scrollToSel = true;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) S.sel = std::max(0, S.sel - page), S.scrollToSel = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) S.cat = (S.cat + (io.KeyShift ? kCategories - 1 : 1)) % kCategories;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (enter && S.sel >= 0 && S.sel < n) choose(a, S.ranked[static_cast<size_t>(S.sel)].index);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      keys::claimEscape();
      closeNow();
    }
    const bool cmd = io.KeyCtrl || io.KeySuper;
    if (S.open && ImGui::GetFrameCount() > S.openFrame + 1 && cmd &&
        (ImGui::IsKeyPressed(ImGuiKey_K, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false))))
      closeNow();
  }
  if (n > 0) S.sel = std::clamp(S.sel, 0, n - 1);

  // ---- rows ----
  const float listTop = wp.y + theme::kS4 + fieldH + theme::kS3 + tabsH + theme::kS2;
  dl->AddLine(ImVec2(wp.x + 1, listTop), ImVec2(wp.x + W - 1, listTop), P.line);
  ImGui::SetCursorScreenPos(ImVec2(wp.x, listTop + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(0));
  ImGui::BeginChild("##cmdrows", ImVec2(W, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
  {
    ImDrawList* rl = ImGui::GetWindowDrawList();
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const float rowW = W - 2 * kPadX;
    const float scrollY = ImGui::GetScrollY();
    if (S.scrollToSel && S.sel >= 0) {
      for (const VItem& it : items)
        if (it.ranked == S.sel) {
          float top = it.y, bot = it.y + it.h;
          if (S.sel == 0) top = 0;  // the first row shows its heading too
          if (top < scrollY) ImGui::SetScrollY(top);
          else if (bot > scrollY + listH) ImGui::SetScrollY(bot - listH);
        }
      S.scrollToSel = false;
    }
    ImGui::Dummy(ImVec2(W, std::max(contentH, 1.0f)));
    const bool hoverList = ImGui::IsWindowHovered();
    const bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    for (const VItem& it : items) {
      if (it.y + it.h < scrollY || it.y > scrollY + listH) continue;
      const ImVec2 r0(cp.x + kPadX, cp.y + it.y), r1(cp.x + kPadX + rowW, cp.y + it.y + it.h);
      if (it.ranked < 0) {
        rl->AddText(a.fontMono, theme::kFontLabel, ImVec2(r0.x + 10, std::round(r1.y - theme::kFontLabel - 6)), P.textFaint, it.head);
        continue;
      }
      const cmdpal::Ranked& rk = S.ranked[static_cast<size_t>(it.ranked)];
      const Entry& e = S.rows[static_cast<size_t>(rk.index)];
      const bool hov = hoverList && ImGui::IsMouseHoveringRect(r0, r1);
      if (hov && moved) S.sel = it.ranked;
      const bool sel = it.ranked == S.sel;
      if (sel) {
        rl->AddRectFilled(r0, r1, P.surface700, 6.0f);
        rl->AddRectFilled(ImVec2(r0.x, r0.y + 7), ImVec2(r0.x + 2, r1.y - 7), P.accent, 1.0f);
      } else if (hov) {
        rl->AddRectFilled(r0, r1, P.surface800, 6.0f);
      }
      if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) choose(a, rk.index);
      const float cy = std::round((r0.y + r1.y) * 0.5f);
      const ImU32 main = e.enabled ? P.text : P.textFaint;
      ui::drawIcon(rl, iconFor(e, S.acts[static_cast<size_t>(rk.index)]), ImVec2(r0.x + 18, cy), 14, e.enabled ? (sel ? P.text : P.textDim) : P.textFaint);
      // Right side: on / off state, then the shortcut.
      float right = r1.x - 10;
      right = drawKeycaps(rl, a, e.shortcut, right, cy, !e.enabled);
      if (!e.shortcut.empty()) right -= theme::kS2;
      if (e.checked) {
        ui::drawIcon(rl, ui::Icon::Check, ImVec2(right - 8, cy), 12, e.enabled ? P.accent : P.textFaint);
        right -= 16 + theme::kS1;
      }
      // Label, then where it lives (or why it cannot run).
      const float lx = r0.x + 36;
      const float room = std::max(40.0f, right - lx - theme::kS2);
      const float fs = ImGui::GetFontSize();
      const float labelW = std::min(ImGui::CalcTextSize(e.label.c_str()).x, room * (e.path.empty() ? 1.0f : 0.62f));
      drawLabel(rl, ImVec2(lx, std::round(cy - fs * 0.5f) - 1), labelW + 1, e.label, rk.hits, main, e.enabled ? P.accent : P.textDim);
      const std::string sub = !e.enabled && !e.hint.empty() ? e.hint : e.path;
      if (!sub.empty()) {
        const float sx = lx + labelW + theme::kS3;
        const float subW = right - sx - theme::kS2;
        if (subW > 24)
          theme::textEllipsis(rl, ImGui::GetFont(), theme::kFontCaption, ImVec2(sx, std::round(cy - theme::kFontCaption * 0.5f) - 1), subW,
                              e.enabled ? P.textFaint : theme::mix(P.textFaint, P.danger, 0.25f), sub.c_str());
      }
      if (hov && !e.hint.empty() && e.enabled) app().setHint(e.hint);
    }
    if (n == 0) {
      const char* t = i18n::tr("Nothing matches");
      const char* b = i18n::tr("Try fewer letters, or another category (Tab)");
      const ImVec2 ts = ImGui::CalcTextSize(t);
      const float ty = cp.y + listH * 0.5f - 18;
      rl->AddText(ImVec2(std::round(cp.x + (W - ts.x) * 0.5f), std::round(ty)), P.textDim, t);
      const ImVec2 bs = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0.0f, b);
      rl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(cp.x + (W - bs.x) * 0.5f), std::round(ty + 24)), P.textFaint, b);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  // ---- footer: the keys ----
  const float footTop = listTop + 1 + listH;
  dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(wp.x + 1, footTop), ImVec2(wp.x + W - 1, footTop), P.line);
  {
    const float cy = std::round(footTop + footH * 0.5f);
    float x = wp.x + theme::kS4;
    auto hintAt = [&](const char* keysText, const char* what) {
      // keycaps left to right
      std::vector<std::string> caps;
      std::string cur;
      for (const char* c = keysText; *c; ++c) {
        if (*c == ' ') {
          if (!cur.empty()) caps.push_back(cur);
          cur.clear();
        } else {
          cur += *c;
        }
      }
      if (!cur.empty()) caps.push_back(cur);
      for (const std::string& k : caps) {
        const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0.0f, k.c_str());
        const float w = std::max(18.0f, std::ceil(ts.x) + 10);
        drawKeycaps(dl, a, k, x + w, cy, true);
        x += w + 3;
      }
      x += 3;
      const ImVec2 ws = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0.0f, what);
      dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(x, std::round(cy - theme::kFontCaption * 0.5f) - 1), P.textFaint, what);
      x += ws.x + theme::kS4;
    };
    hintAt("Up Down", i18n::tr("Move"));
    hintAt("Enter", i18n::tr("Run"));
    hintAt("Tab", i18n::tr("Category"));
    // The selected row's own description, right-aligned when it fits.
    if (S.sel >= 0 && S.sel < n) {
      const Entry& e = S.rows[static_cast<size_t>(S.ranked[static_cast<size_t>(S.sel)].index)];
      const char* what = e.kind == Kind::Window     ? i18n::tr("Opens the window")
                         : e.kind == Kind::Setting  ? i18n::tr("Opens Settings at it")
                         : e.kind == Kind::Plugin   ? i18n::tr("Adds it")
                         : e.enabled                ? i18n::tr("Runs it")
                                                    : i18n::tr("Not available now");
      const ImVec2 ws = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0.0f, what);
      const float wx = wp.x + W - theme::kS4 - ws.x;
      if (wx > x) dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(wx), std::round(cy - theme::kFontCaption * 0.5f) - 1), P.textDim, what);
    }
  }
  // A click elsewhere (another window took the focus): close.
  if (!S.wantFocus && ImGui::GetFrameCount() > S.openFrame + 2 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::GetTopMostPopupModal() == nullptr)
    closeNow();
  ImGui::End();
  S.wantFocus = false;
}

void devDump(App& a, int frame) {
  std::printf("PALETTE BEGIN frame %d layout %s rows %zu\n", frame, layout::id(layoutui::menuStyle(a)), S.rows.size());
  static const char* kinds[] = {"command", "window", "setting", "plugin"};
  for (const Entry& e : S.rows)
    std::printf("ROW\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\n", kinds[static_cast<int>(e.kind)], e.id.c_str(), e.commandId.empty() ? "-" : e.commandId.c_str(),
                e.enabled ? 1 : 0, e.label.c_str(), e.labelEn.empty() ? e.label.c_str() : e.labelEn.c_str(), e.path.c_str(), e.shortcut.c_str());
  for (const MenuRec& r : S.menuRecs) std::printf("MENU\t%s\t%s\t%s\t%d\t%s\n", r.rowId.c_str(), join(r.en, " > ").c_str(), r.labelEn.c_str(), r.occ, r.label.c_str());
  for (const keys::Command& c : keys::commands()) std::printf("KEYCMD\t%s\n", c.id);
  std::printf("PALETTE END\n");
  std::fflush(stdout);
}

void devStateLine(App& a, int frame) {
  std::string top;
  for (size_t i = 0; i < S.ranked.size() && i < 5; ++i) top += (i ? " ;; " : "") + S.rows[static_cast<size_t>(S.ranked[i].index)].id;
  std::string recentLine;  // one per line in the setting, " ;; " between them here
  for (size_t i = 0; i < S.recent.size(); ++i) recentLine += (i ? " ;; " : "") + S.recent[i];
  const std::string sel = S.sel >= 0 && S.sel < static_cast<int>(S.ranked.size()) ? S.rows[static_cast<size_t>(S.ranked[static_cast<size_t>(S.sel)].index)].id : "-";
  std::printf("palette frame %d: open %d cat %d query [%s] sel %s top %s recent %s | metronome %d mixer %d settings %d highlight [%s] position %lld channels %zu\n",
              frame, S.open ? 1 : 0, S.cat, S.query, sel.c_str(), top.empty() ? "-" : top.c_str(),
              S.recent.empty() ? "-" : recentLine.c_str(), a.metronome ? 1 : 0, a.showMixer ? 1 : 0, a.showSettings ? 1 : 0,
              gHi.label.c_str(), static_cast<long long>(a.engine.position()), a.doc.song().channels.size());
  std::fflush(stdout);
}

}  // namespace

// ------------------------------------------------------------- public ----
SkipRows::SkipRows() { ++gSkip; }
SkipRows::~SkipRows() { --gSkip; }

namespace {

void takeHint(const char* text) {
  if (!gListing || !gListing->out || !gListing->lastWasItem || gListing->out->empty()) return;
  if (ImGui::GetItemID() != gListing->lastItem) return;
  MenuRec& r = gListing->out->back();
  if (r.hint.empty()) r.hint = i18n::tr(text);
}

void takeSection(const char* captionEn) {
  if (gSet.on) gSet.section = captionEn ? captionEn : "";
}

void takeFormLabel(const char* shown) {
  if (gSet.on) {
    addSetting(shown);
    return;
  }
  if (shown && !gHi.label.empty() && ImGui::GetTime() < gHi.until && cmdpal::stripId(shown) == gHi.label && inSettingsWindow()) {
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    lightRow(ImVec2(c.x - 8, c.y - 3), ImVec2(w->WorkRect.Max.x + 4, c.y + ImGui::GetFrameHeight() + 3));
  }
}

// The shared widgets reach these through detail:: (header-only, no link to this file).
const struct NoteTakers {
  NoteTakers() {
    detail::hintFn = takeHint;
    detail::sectionFn = takeSection;
    detail::formLabelFn = takeFormLabel;
  }
} gNoteTakers;

}  // namespace

void open(App& a) {
  if (S.open) return;
  S.open = true;
  S.wantFocus = true;
  S.needBuild = true;
  S.openFrame = ImGui::GetFrameCount();
  S.query[0] = 0;
  S.cat = 0;
  S.sel = 0;
  S.recent = cmdpal::parseRecent(a.settings.getString("ui.paletteRecent"));
  S.rankedQuery = "\x01";
  S.prevFocus.clear();
  if (ImGuiWindow* nw = GImGui->NavWindow) S.prevFocus = nw->RootWindow ? nw->RootWindow->Name : nw->Name;
}

void close() { closeNow(); }

void toggle(App& a) {
  if (S.open) closeNow();
  else open(a);
}

bool isOpen() { return S.open; }

std::vector<CommandInfo> commandList(App& a) {
  build(a);
  std::vector<CommandInfo> out;
  out.reserve(S.rows.size());
  for (const Entry& e : S.rows) out.push_back({e.id, e.label, e.path, e.shortcut, e.enabled, e.kind});
  return out;
}

bool run(App& a, const std::string& id) {
  if (!S.built) build(a);
  for (size_t i = 0; i < S.rows.size(); ++i)
    if (S.rows[i].id == id) {
      if (!S.rows[i].enabled) return false;
      S.pending = true;
      S.pendingAct = S.acts[i];
      return true;
    }
  return false;
}

void draw(App& a) {
  // Lit settings rows and walks need the widget notes; nothing else does.
  const bool lit = !gHi.label.empty() && ImGui::GetTime() < gHi.until;
  if (!lit && !gHi.label.empty()) gHi.label.clear();
  detail::noting = lit;
  imhook::setInfoHook(lit ? infoHook : nullptr);

  if (S.pending) {
    S.pending = false;
    perform(a, S.pendingAct);
  }
  tickInject();

  if (S.open && S.needBuild) {
    build(a);
    S.needBuild = false;
  }
  if (S.devDump || !S.devRun.empty()) {
    if (!S.built) build(a);
    if (S.devDump) devDump(a, S.devFrame);
    if (!S.devRun.empty() && !run(a, S.devRun)) {
      std::printf("palette frame %d: no row %s to run\n", S.devFrame, S.devRun.c_str());
      std::fflush(stdout);
    }
    S.devDump = false;
    S.devRun.clear();
  }
  detail::noting = lit;
  imhook::setInfoHook(lit ? infoHook : nullptr);
  if (S.open) drawPanel(a);
  if (S.devState) {
    rerank();
    devStateLine(a, S.devFrame);
    S.devState = false;
  }
  if (!S.open) S.wantFocus = false;
}

bool devCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return false;
  const std::string& c = args[0];
  S.devFrame = frame;
  if (c == "open") open(a);
  else if (c == "close") closeNow();
  else if (c == "dump") S.devDump = true;
  else if (c == "state") S.devState = true;
  else if (c == "run" && args.size() > 1) {  // ids may hold spaces
    S.devRun.clear();
    for (size_t i = 1; i < args.size(); ++i) S.devRun += (i > 1 ? " " : "") + args[i];
  }
  else if (c == "cat" && args.size() > 1) S.cat = std::clamp(std::atoi(args[1].c_str()), 0, kCategories - 1);
  else return false;
  return true;
}

}  // namespace gl::cmdpalette

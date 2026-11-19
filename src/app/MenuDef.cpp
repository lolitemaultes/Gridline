// Gridline - the menus as data (K-112). See MenuDef.h.
#include "MenuDef.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include "App.h"
#include "Keys.h"
#include "LayoutStyleUi.h"
#include "LogicPlugins.h"
#include "Widgets.h"
#include "WindowRaise.h"
#include "Windows.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::menudef {

Entry item(const char* label, const char* key, Fn run) {
  Entry e;
  e.kind = Kind::Item;
  e.label = label;
  e.key = key;
  e.run = run;
  return e;
}

Entry item(const char* label, Fn run) { return item(label, nullptr, run); }

Entry toggle(const char* label, const char* key, Flag flag) {
  Entry e;
  e.kind = Kind::Toggle;
  e.label = label;
  e.key = key;
  e.flag = flag;
  return e;
}

Entry separator() {
  Entry e;
  e.kind = Kind::Separator;
  return e;
}

Entry heading(const char* label) {
  Entry e;
  e.kind = Kind::Heading;
  e.label = label;
  return e;
}

Entry submenu(const char* label, Menu children) {
  Entry e;
  e.kind = Kind::Submenu;
  e.label = label;
  e.children = std::move(children);
  return e;
}

Entry provider(Fn code) {
  Entry e;
  e.kind = Kind::Provider;
  e.run = code;
  return e;
}

namespace {

// K-112D (audit I2): on macOS, Smooth's menus write a shortcut as the system does: the modifier glyphs
// in the order Control, Option, Shift, Command, then the key ("Cmd+Alt+N" shows as "⌥⌘N", "Shift+Enter"
// as "⇧↩"). Anything else (two shortcuts, a mouse gesture) stays as written. Made once per text.
const char* macGlyphs(const char* sc) {
  if (!sc || !*sc) return sc;
  static std::unordered_map<std::string, std::string> cache;
  auto it = cache.find(sc);
  if (it != cache.end()) return it->second.c_str();
  bool ctrl = false, alt = false, shift = false, cmd = false;
  const char* r = sc;
  for (bool more = true; more;) {
    more = false;
    for (auto [pre, flag] : {std::pair<const char*, bool*>{"Ctrl+", &ctrl}, {"Cmd+", &cmd}, {"Alt+", &alt}, {"Option+", &alt}, {"Shift+", &shift}})
      if (std::strncmp(r, pre, std::strlen(pre)) == 0 && r[std::strlen(pre)] != '\0') {
        *flag = true;
        r += std::strlen(pre);
        more = true;
      }
  }
  static const std::pair<const char*, const char*> kKeys[] = {
      {"Enter", "\xE2\x86\xA9"},     {"Return", "\xE2\x86\xA9"}, {"Backspace", "\xE2\x8C\xAB"}, {"Del", "\xE2\x8C\xA6"},
      {"Delete", "\xE2\x8C\xA6"},    {"Esc", "\xE2\x8E\x8B"},    {"Tab", "\xE2\x87\xA5"},       {"Up", "\xE2\x86\x91"},
      {"Down", "\xE2\x86\x93"},      {"Left", "\xE2\x86\x90"},   {"Right", "\xE2\x86\x92"},     {"PgUp", "\xE2\x87\x9E"},
      {"PgDn", "\xE2\x87\x9F"},      {"Home", "\xE2\x86\x96"},   {"End", "\xE2\x86\x98"}};
  std::string key = r;
  for (const auto& [name, glyph] : kKeys)
    if (key == name) key = glyph;
  // a key name with a space or a comma left (two shortcuts, "Num +", a gesture): the text as it was.
  // hunt-1119: the comma key itself is a key ("Cmd+," is Settings...: it showed as "Cmd+," among glyphs).
  const bool plain = (ctrl || alt || shift || cmd || key != r) && (key == "," || key.find_first_of(" ,") == std::string::npos);
  std::string out;
  if (plain) {
    if (ctrl) out += "\xE2\x8C\x83";
    if (alt) out += "\xE2\x8C\xA5";
    if (shift) out += "\xE2\x87\xA7";
    if (cmd) out += "\xE2\x8C\x98";
    out += key;
  } else {
    out = sc;
  }
  return cache.emplace(sc, std::move(out)).first->second.c_str();
}

}  // namespace

void emit(App& a, const Menu& m, imhook::MenuWalker* w) {
  std::string made;
#ifdef __APPLE__
  const bool glyphs = !w && logicplug::active(a);
#else
  const bool glyphs = false;
#endif
  for (const Entry& e : m) {
    if (e.shown && !e.shown(a)) continue;
    const char* lab = nullptr;
    if (e.dynLabel) {
      made = e.dynLabel(a);
      lab = made.c_str();
    } else if (e.label) {
      lab = i18n::tr(e.label);
    }
    // K-106: the layout's own words reach the menu tree (the walk) and the headings too.
    if (GImGuiGridlineMenuLabel && lab) lab = GImGuiGridlineMenuLabel(lab);
    switch (e.kind) {
      case Kind::Item: {
        const char* sc = e.dynKey ? e.dynKey(a) : e.key ? keys::label(e.key) : nullptr;
        if (w && e.key && !sc && e.keyHidden) {
          w->nextCommand(e.key);  // K-314: a hidden shortcut keeps the item's command (one palette row)
        }
        if (glyphs) sc = macGlyphs(sc);
        const bool chk = e.checked && e.checked(a);
        const bool en = !e.enabled || e.enabled(a);
        const bool chosen = w ? w->menuItem(lab, sc, chk, en) : ImGui::MenuItem(lab, sc, chk, en);
        if (chosen && e.run) e.run(a);
        if (e.hint) ui::Hint(e.hint);
        break;
      }
      case Kind::Toggle: {
        const char* sc = e.dynKey ? e.dynKey(a) : e.key ? keys::label(e.key) : nullptr;
        if (w && e.key && !sc && e.keyHidden) {
          w->nextCommand(e.key);
        }
        if (glyphs) sc = macGlyphs(sc);
        bool* f = e.flag(a);
        // K-297 (round 6): a window open as a tab hidden behind another comes forward (it closed)
        if (w ? w->menuItem(lab, sc, *f, true) : ImGui::MenuItem(lab, sc, *f))
          if (!*f || !ui::bringTabForward(e.label)) {
            *f = !*f;
            // K-297 (round 8): a window the menu opens takes the keys, as its key does (View > Mixer
            // in Smooth and Clips opened the band with the keys left in the tracks).
            if (*f && layout::findWindow(e.label)) a.focusWindow = e.label;
          }
        if (e.hint) ui::Hint(e.hint);
        break;
      }
      case Kind::Separator:
        if (w) w->separator(nullptr);
        else ImGui::Separator();
        break;
      case Kind::Heading:
        if (w) w->separator(lab);
        else if (layoutui::menuTitles()) ImGui::SeparatorText(lab);
        else if (&e != &m.front() && (&e - 1)->kind != Kind::Separator) ImGui::Separator();  // K-112 (Live): a plain line, never first or doubled
        break;
      case Kind::Submenu: {
        const bool en = !e.enabled || e.enabled(a);
        if (w) {
          if (w->beginMenu(lab, en)) {
            emit(a, e.children, w);
            w->endMenu();
          }
          break;
        }
        const bool open = ImGui::BeginMenu(lab, en);
        if (e.devNote) devNoteRect(e.devNote);
        if (e.devRect) devRackItem(e.devRect);
        if (open) {
          emit(a, e.children, nullptr);
          ImGui::EndMenu();
        }
        break;
      }
      case Kind::Provider: {
        if (!e.run) break;
        imhook::MenuWalker* was = imhook::menuWalker();
        const bool swap = w && w != was;  // drawn: whatever walker is set stays
        if (swap) imhook::setMenuWalker(w);
        e.run(a);
        if (swap) imhook::setMenuWalker(was);
        break;
      }
    }
  }
}

int count(const Menu& m, Kind k) {
  int n = 0;
  for (const Entry& e : m) {
    if (e.kind == k) ++n;
    n += count(e.children, k);
  }
  return n;
}

}  // namespace gl::menudef

// Gridline - the menus as data (K-112). See MenuDef.h.
#include "MenuDef.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "Keys.h"
#include "LayoutStyleUi.h"
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

void emit(App& a, const Menu& m, imhook::MenuWalker* w) {
  std::string made;
  for (const Entry& e : m) {
    if (e.shown && !e.shown(a)) continue;
    const char* lab = nullptr;
    if (e.dynLabel) {
      made = e.dynLabel(a);
      lab = made.c_str();
    } else if (e.label) {
      lab = i18n::tr(e.label);
    }
    switch (e.kind) {
      case Kind::Item: {
        const char* sc = e.dynKey ? e.dynKey(a) : e.key ? keys::label(e.key) : nullptr;
        const bool chk = e.checked && e.checked(a);
        const bool en = !e.enabled || e.enabled(a);
        const bool chosen = w ? w->menuItem(lab, sc, chk, en) : ImGui::MenuItem(lab, sc, chk, en);
        if (chosen && e.run) e.run(a);
        if (e.hint) ui::Hint(e.hint);
        break;
      }
      case Kind::Toggle: {
        const char* sc = e.dynKey ? e.dynKey(a) : e.key ? keys::label(e.key) : nullptr;
        bool* f = e.flag(a);
        // K-297 (round 6): a window open as a tab hidden behind another comes forward (it closed)
        if (w ? w->menuItem(lab, sc, *f, true) : ImGui::MenuItem(lab, sc, *f))
          if (!*f || !ui::bringTabForward(e.label)) *f = !*f;
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

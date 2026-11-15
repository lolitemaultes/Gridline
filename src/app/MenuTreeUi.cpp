// Gridline - the menu tree read from the menu definitions (K-112). See MenuTreeUi.h.
#include "MenuTreeUi.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include "../../third_party/imgui/imgui.h"
#include "../core/CommandPalette.h"
#include "../core/I18n.h"
#include "../core/MenuStyle.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "../core/Keymap.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::menutreeui {

namespace {

// Draws `f` into a window nobody sees: fully clipped, no inputs, no saved settings.
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

// The keymap command behind a menu item's shortcut text.
const char* commandOf(const char* shortcut) {
  if (!shortcut || !*shortcut) return nullptr;
  if (const char* c = keys::commandForLabel(shortcut)) return c;
  const keys::Chord ch = keys::parse(shortcut);
  if (!ch.valid()) return nullptr;
  const auto v = keys::keymap().lookup(keys::Context::Global, ch);
  return v.size() == 1 ? v[0]->id : nullptr;
}

const char* rootName(const menustyle::Menu& m) { return m.name ? m.name : "Gridline"; }

struct TreeWalker : imhook::MenuWalker {
  std::vector<menutree::Node*> stack;  // [0] is the root menu
  std::map<std::string, int> seen;     // path + label -> occurrences so far
  std::string pathKey;
  bool pendingSeparator = false;
  menutree::Node* last = nullptr;  // the item just walked (for its hint)
  ImGuiID lastId = 0;

  menutree::Node& add(menutree::Node n) {
    n.separatorBefore = pendingSeparator;
    pendingSeparator = false;
    stack.back()->children.push_back(std::move(n));
    return stack.back()->children.back();
  }
  static void names(menutree::Node& n, const char* label) {
    n.raw = label ? label : "";
    n.shown = cmdpal::stripId(n.raw);
    const char* en = i18n::source(label);
    n.en = cmdpal::stripId(en ? en : n.raw);
  }
  bool beginMenu(const char* label, bool enabled) override {
    menutree::Node n;
    names(n, label);
    n.submenu = true;
    n.enabled = enabled && stack.back()->enabled;
    menutree::Node& added = add(std::move(n));
    stack.push_back(&added);
    last = nullptr;
    return true;
  }
  void endMenu() override {
    if (stack.size() > 1) stack.pop_back();
    last = nullptr;
  }
  bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled) override {
    last = nullptr;
    if (!label) return false;
    menutree::Node n;
    names(n, label);
    if (n.shown.empty()) return false;
    n.shortcut = shortcut ? shortcut : "";
    if (const char* c = commandOf(shortcut)) n.command = c;
    n.checked = selected;
    n.paletteSkip = cmdpalette::skippingRows();
    n.enabled = enabled && stack.back()->enabled;
    std::string key;
    for (size_t i = 1; i < stack.size(); ++i) key += stack[i]->raw + "\x1f";
    n.occ = seen[key + n.raw]++;
    last = &add(std::move(n));
    lastId = ImGui::GetItemID();
    return false;
  }
  void separator(const char*) override {
    if (stack.size() > 0 && !stack.back()->children.empty()) pendingSeparator = true;
  }
};

TreeWalker* gWalking = nullptr;

struct RunWalker : imhook::MenuWalker {
  std::vector<std::string> raw;
  std::string label;
  int occ = 0, seenCount = 0;
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
    if (done || !l || depth != raw.size() || label != l) return false;
    if (seenCount++ != occ) return false;
    done = true;
    if (!enabled) {
      blocked = true;
      return false;
    }
    return true;
  }
};

}  // namespace

menutree::Tree build(App& a, layout::Preset style) {
  menutree::Tree t;
  t.style = layout::id(style);
  const auto& menus = menustyle::menus(style);
  unseen("##menutree_walk", [&] {
    for (const menustyle::Menu& m : menus) {
      menutree::Node root;
      const char* name = rootName(m);
      root.raw = name;
      root.shown = cmdpal::stripId(i18n::tr(name));
      root.en = name;
      root.submenu = true;
      t.roots.push_back(std::move(root));
    }
    for (size_t i = 0; i < menus.size(); ++i) {
      TreeWalker w;
      w.stack.push_back(&t.roots[i]);
      gWalking = &w;
      const bool wasNoting = cmdpalette::detail::noting;
      cmdpalette::detail::noting = true;  // ui::Hint reports the items' descriptions
      imhook::setMenuWalker(&w);
      drawStyleMenu(a, menus[i], &w);  // read from the menu definitions (MenuDef.h)
      imhook::setMenuWalker(nullptr);
      cmdpalette::detail::noting = wasNoting;
      gWalking = nullptr;
    }
  });
  return t;
}

menutree::Tree build(App& a) { return build(a, layoutui::menuStyle(a)); }

bool noteHint(const char* text) {
  if (!gWalking) return false;
  if (gWalking->last && ImGui::GetItemID() == gWalking->lastId && gWalking->last->hint.empty() && text) gWalking->last->hint = i18n::tr(text);
  return true;
}

RunResult run(App& a, layout::Preset style, int root, const std::vector<std::string>& rawPath, const std::string& rawLabel, int occ) {
  const auto& menus = menustyle::menus(style);
  if (root < 0 || root >= static_cast<int>(menus.size())) return RunResult::Gone;
  RunWalker w;
  w.raw = rawPath;
  w.label = rawLabel;
  w.occ = occ;
  unseen("##menutree_run", [&] {
    imhook::setMenuWalker(&w);
    drawStyleMenu(a, menus[static_cast<size_t>(root)], &w);  // the chosen entry runs from its definition
    imhook::setMenuWalker(nullptr);
  });
  if (!w.done) return RunResult::Gone;
  return w.blocked ? RunResult::Disabled : RunResult::Ran;
}

RunResult run(App& a, layout::Preset style, const menutree::Ref& item) {
  if (!item.item) return RunResult::Gone;
  return run(a, style, item.root, item.rawPath(), item.item->raw, item.item->occ);
}

namespace {
std::vector<layout::Preset> gDumps;  // asked by the dev input (before the frame), printed in it
}

bool devCommand(App& a, const std::vector<std::string>& args, int) {
  if (args.empty() || args[0] != "dump") return false;
  gDumps.push_back(args.size() > 1 ? layout::fromId(args[1]) : layoutui::menuStyle(a));
  return true;
}

void devFrame(App& a, int frame) {
  for (const layout::Preset style : std::exchange(gDumps, {})) {
    const menutree::Tree t = build(a, style);
    std::printf("MENUTREE BEGIN frame %d style %s items %d submenus %d\n%sMENUTREE END\n", frame, t.style.c_str(), menutree::countItems(t),
                menutree::countSubmenus(t), menutree::dump(t).c_str());
  }
  std::fflush(stdout);
}

}  // namespace gl::menutreeui

// Gridline — J-92: Dear ImGui's item hooks (built with IMGUI_ENABLE_TEST_ENGINE),
// forwarded to callbacks: the editor audit's (--layout-audit: the binding audit
// clicks every item of an editor to reach its pages and tabs) and, J-111d, the
// collaborator cursors' (CursorAnchor.h: the item under a pointer). Off (the
// normal case) costs one branch per item: ImGuiContext::TestEngineHookItems.
#pragma once
#include "imgui.h"

namespace gl::imhook {

using ItemFn = void (*)(ImGuiID id, ImVec2 min, ImVec2 max);
// Calls `fn` for every item ImGui adds from now on (nullptr = off).
void setItemHook(ItemFn fn);
// J-111d: a second, independent hook (collaborator cursors); both may be on.
void setCursorHook(ItemFn fn);

// K-93: every item's label and status flags (ImGuiItemStatusFlags), right after the item
// (ImGui::GetItemRectMin/Max are the item's). nullptr = off.
using InfoFn = void (*)(ImGuiID id, const char* label, int statusFlags);
void setInfoHook(InfoFn fn);

// K-93: the command palette's menu walk. While a walker is set, ImGui::BeginMenu() /
// EndMenu() / MenuItem() call it instead of drawing anything: BeginMenu returns what
// beginMenu returns (true: the caller draws the submenu's items), MenuItem returns what
// menuItem returns (true: the caller runs the item's action), EndMenu closes the submenu.
struct MenuWalker {
  virtual ~MenuWalker() = default;
  virtual bool beginMenu(const char* label, bool enabled) = 0;
  virtual void endMenu() = 0;
  virtual bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled) = 0;
};
void setMenuWalker(MenuWalker* w);
MenuWalker* menuWalker();

}  // namespace gl::imhook

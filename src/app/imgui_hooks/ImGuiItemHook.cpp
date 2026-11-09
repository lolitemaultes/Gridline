// Gridline — J-92: Dear ImGui item hooks, see ImGuiItemHook.h. Part of the
// imgui_core library (the hook symbols ImGui calls must live next to it).
#include "ImGuiItemHook.h"
#include "imgui_internal.h"

bool GImGuiGridlineMenuWalk = false;  // K-93: read by imgui_widgets.cpp (BeginMenuEx, EndMenu, MenuItemEx)

namespace gl::imhook {
namespace {
ItemFn itemFn = nullptr;
ItemFn cursorFn = nullptr;
InfoFn infoFn = nullptr;
MenuWalker* walker = nullptr;

void update() {
  if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = itemFn != nullptr || cursorFn != nullptr || infoFn != nullptr;
}
}  // namespace

void setItemHook(ItemFn fn) {
  itemFn = fn;
  update();
}

void setCursorHook(ItemFn fn) {
  cursorFn = fn;
  update();
}

void setInfoHook(InfoFn fn) {
  infoFn = fn;
  update();
}

void setMenuWalker(MenuWalker* w) {
  walker = w;
  GImGuiGridlineMenuWalk = w != nullptr;
}

MenuWalker* menuWalker() { return walker; }

}  // namespace gl::imhook

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
  if (gl::imhook::itemFn) gl::imhook::itemFn(id, bb.Min, bb.Max);
  if (gl::imhook::cursorFn) gl::imhook::cursorFn(id, bb.Min, bb.Max);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
  if (gl::imhook::infoFn) gl::imhook::infoFn(id, label, static_cast<int>(flags));
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

bool ImGuiGridlineHook_BeginMenu(const char* label, bool enabled) {
  return gl::imhook::walker ? gl::imhook::walker->beginMenu(label, enabled) : false;
}
void ImGuiGridlineHook_EndMenu() {
  if (gl::imhook::walker) gl::imhook::walker->endMenu();
}
bool ImGuiGridlineHook_MenuItem(const char* label, const char* shortcut, bool selected, bool enabled) {
  return gl::imhook::walker ? gl::imhook::walker->menuItem(label, shortcut, selected, enabled) : false;
}

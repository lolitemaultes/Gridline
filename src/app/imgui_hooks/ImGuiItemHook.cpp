// Gridline — J-92: Dear ImGui item hooks, see ImGuiItemHook.h. Part of the
// imgui_core library (the hook symbols ImGui calls must live next to it).
#include "ImGuiItemHook.h"
#include "imgui_internal.h"

bool GImGuiGridlineMenuRight = false;  // K-112b: read by imgui_widgets.cpp (MenuItemEx), set by layoutui
ImFont* GImGuiGridlineTitleFont = nullptr;  // K-112b (checkpoint 28): read by imgui.cpp (RenderWindowTitleBarContents), set by layoutui
float GImGuiGridlineCheckRounding = -1.0f;  // checkpoint 38: read by imgui_widgets.cpp (Checkbox), set by layoutui
float GImGuiGridlineRadioDot = -1.0f;  // smooth-radio: read by imgui_widgets.cpp (RadioButton), set by layoutui
ImU32 GImGuiGridlineMenuHover = 0;      // K-112B checkpoint 39: read by imgui_widgets.cpp (MenuItemEx, BeginMenuEx)
bool GImGuiGridlineMenuWalk = false;  // K-93: read by imgui_widgets.cpp (BeginMenuEx, EndMenu, MenuItemEx)
const char* (*GImGuiGridlineMenuLabel)(const char*) = nullptr;  // K-106: read by imgui_widgets.cpp (BeginMenuEx, MenuItemEx), set by layoutui

namespace gl::imhook {
namespace {
ItemFn itemFn = nullptr;
ItemFn cursorFn = nullptr;
ItemFn typeInFn = nullptr;
InfoFn infoFn = nullptr;
InfoFn devLabelFn = nullptr;
ItemFn sweepItemFn = nullptr;
InfoFn sweepInfoFn = nullptr;
MenuWalker* walker = nullptr;

void update() {
  if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = itemFn != nullptr || cursorFn != nullptr || typeInFn != nullptr || infoFn != nullptr ||
                                                                 devLabelFn != nullptr || sweepItemFn != nullptr || sweepInfoFn != nullptr;
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

void setTypeInHook(ItemFn fn) {
  typeInFn = fn;
  update();
}

void setInfoHook(InfoFn fn) {
  infoFn = fn;
  update();
}

void setDevLabelHook(InfoFn fn) {
  devLabelFn = fn;
  update();
}

void setSweepHooks(ItemFn item, InfoFn info) {
  sweepItemFn = item;
  sweepInfoFn = info;
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
  if (gl::imhook::typeInFn) gl::imhook::typeInFn(id, bb.Min, bb.Max);
  if (gl::imhook::sweepItemFn) gl::imhook::sweepItemFn(id, bb.Min, bb.Max);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
  if (gl::imhook::infoFn) gl::imhook::infoFn(id, label, static_cast<int>(flags));
  if (gl::imhook::devLabelFn) gl::imhook::devLabelFn(id, label, static_cast<int>(flags));
  if (gl::imhook::sweepInfoFn) gl::imhook::sweepInfoFn(id, label, static_cast<int>(flags));
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
void ImGuiGridlineHook_Separator(const char* text) {
  if (gl::imhook::walker) gl::imhook::walker->separator(text);
}

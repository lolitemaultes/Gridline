// Gridline — J-92: Dear ImGui item hooks, see ImGuiItemHook.h. Part of the
// imgui_core library (the hook symbols ImGui calls must live next to it).
#include "ImGuiItemHook.h"
#include "imgui_internal.h"

namespace gl::imhook {
namespace {
ItemFn itemFn = nullptr;
ItemFn cursorFn = nullptr;

void update() {
  if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = itemFn != nullptr || cursorFn != nullptr;
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

}  // namespace gl::imhook

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
  if (gl::imhook::itemFn) gl::imhook::itemFn(id, bb.Min, bb.Max);
  if (gl::imhook::cursorFn) gl::imhook::cursorFn(id, bb.Min, bb.Max);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID, const char*, ImGuiItemStatusFlags) {}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

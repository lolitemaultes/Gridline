// Gridline — J-92: Dear ImGui item hooks, see ImGuiItemHook.h. Part of the
// imgui_core library (the hook symbols ImGui calls must live next to it).
#include "ImGuiItemHook.h"
#include "imgui_internal.h"

namespace gl::imhook {
namespace {
ItemFn itemFn = nullptr;
}

void setItemHook(ItemFn fn) {
  itemFn = fn;
  if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = fn != nullptr;
}

}  // namespace gl::imhook

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
  if (gl::imhook::itemFn) gl::imhook::itemFn(id, bb.Min, bb.Max);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID, const char*, ImGuiItemStatusFlags) {}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

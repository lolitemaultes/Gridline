// Gridline — J-92: Dear ImGui's item hooks (built with IMGUI_ENABLE_TEST_ENGINE),
// forwarded to a callback the editor audit sets (--layout-audit: the binding
// audit clicks every item of an editor to reach its pages and tabs). Off (the
// normal case) costs one branch per item: ImGuiContext::TestEngineHookItems.
#pragma once
#include "imgui.h"

namespace gl::imhook {

using ItemFn = void (*)(ImGuiID id, ImVec2 min, ImVec2 max);
// Calls `fn` for every item ImGui adds from now on (nullptr = off).
void setItemHook(ItemFn fn);

}  // namespace gl::imhook

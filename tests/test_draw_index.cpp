// Gridline: K-24 tests: large draw lists stay intact when the renderer cannot honour draw command
// vertex offsets (OpenGL 3.0/3.1 contexts, seen on Windows drivers). A busy playlist can pass 65535
// vertices in one window; Gridline builds ImGui with 32-bit indices so every index still points at
// its own vertex, with no vertex offset needed. Headless: no GL context.
#include "../third_party/imgui/imgui.h"
#include "Check.h"

int main() {
  expect(sizeof(ImDrawIdx) == 4, "ImGui draws with 32-bit indices");

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // headless: textures are never uploaded
  io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;  // as on a GL 3.0 driver
  io.DisplaySize = ImVec2(1600, 1000);
  io.DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(1600, 1000));
  ImGui::Begin("busy");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // About 30000 filled rects and 3000 polylines: well past 65535 vertices, like many automation lanes.
  for (int i = 0; i < 30000; ++i) {
    const float x = static_cast<float>(i % 400) * 4.0f, y = static_cast<float>(i / 400) * 12.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 3, y + 10), IM_COL32(200, 120, 40, 255));
  }
  for (int i = 0; i < 3000; ++i) {
    const float y = static_cast<float>(i % 900);
    dl->AddLine(ImVec2(0, y), ImVec2(1600, y + 5), IM_COL32_WHITE, 1.5f);
  }
  ImGui::End();
  ImGui::Render();

  const ImDrawData* dd = ImGui::GetDrawData();
  bool large = false, inRange = true, noOffset = true, reachesEnd = true;
  for (const ImDrawList* list : dd->CmdLists) {
    if (list->VtxBuffer.Size > 65535) large = true;
    unsigned top = 0;
    for (const ImDrawCmd& c : list->CmdBuffer) {
      if (c.VtxOffset != 0) noOffset = false;
      for (unsigned k = 0; k < c.ElemCount; ++k) {
        const unsigned v = static_cast<unsigned>(list->IdxBuffer[static_cast<int>(c.IdxOffset + k)]);
        if (static_cast<int>(v) >= list->VtxBuffer.Size) inRange = false;
        if (v > top) top = v;
      }
    }
    // Wrapped 16-bit indices never reach past vertex 65535: the last vertices would draw from the first.
    if (list->VtxBuffer.Size > 0 && static_cast<int>(top) != list->VtxBuffer.Size - 1) reachesEnd = false;
  }
  expect(large, "the test window passes 65535 vertices");
  expect(noOffset, "no draw command needs a vertex offset");
  expect(reachesEnd, "the last vertex is drawn by its own index");
  expect(inRange, "every index points inside its draw list's vertices (no wrap)");
  ImGui::DestroyContext();
  return finish();
}

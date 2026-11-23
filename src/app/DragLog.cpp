// Gridline - 0.7.8: the drag log (DragLog.h).
#include "DragLog.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace gl::draglog {
namespace {
std::FILE* gFile = nullptr;
bool gChecked = false;
std::vector<std::string> gLines;  // this frame's notes
int gAfter = 0;                   // frames still logged after the buttons came up
size_t gDebugSeen = 0;

const char* nameOf(const ImGuiWindow* w) { return w ? w->Name : "-"; }

void activeHook(ImGuiID oldId, ImGuiID newId, ImGuiWindow* window) {
  if (!gFile) return;
  ImGuiContext& g = *GImGui;
  std::fprintf(gFile, "frame %d ACTIVE %08X (%s) -> %08X (%s) mouse down %d %d\n", g.FrameCount, oldId, nameOf(g.ActiveIdWindow), newId, nameOf(window),
               g.IO.MouseDown[0] ? 1 : 0, g.IO.MouseDown[1] ? 1 : 0);
#if !defined(_WIN32)
  void* frames[24];
  const int n = backtrace(frames, 24);
  for (int i = 1; i < n; ++i) {
    Dl_info info{};
    if (dladdr(frames[i], &info) && info.dli_fbase) {
      std::fprintf(gFile, "    at %s +0x%lx (%s +0x%lx)\n", info.dli_sname ? info.dli_sname : "?",
                   info.dli_saddr ? static_cast<unsigned long>(static_cast<char*>(frames[i]) - static_cast<char*>(info.dli_saddr)) : 0ul,
                   info.dli_fname ? info.dli_fname : "?", static_cast<unsigned long>(static_cast<char*>(frames[i]) - static_cast<char*>(info.dli_fbase)));
    } else {
      std::fprintf(gFile, "    at %p\n", frames[i]);
    }
  }
#endif
  std::fflush(gFile);
}
}  // namespace

bool on() {
  if (!gChecked) {
    gChecked = true;
    if (const char* p = std::getenv("GRIDLINE_DRAGLOG"); p && *p) gFile = std::fopen(p, "w");
  }
  return gFile != nullptr;
}

void install() {
  if (!on() || !ImGui::GetCurrentContext()) return;
  ImGuiContext& g = *GImGui;
  g.ActiveIdHook = activeHook;
  g.DebugLogFlags |= ImGuiDebugLogFlags_EventActiveId | ImGuiDebugLogFlags_EventFocus | ImGuiDebugLogFlags_EventPopup | ImGuiDebugLogFlags_EventViewport;
  std::fprintf(gFile, "drag log: imgui %s\n", ImGui::GetVersion());
  std::fflush(gFile);
}

void note(const char* fmt, ...) {
  if (!gFile) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  gLines.emplace_back(buf);
}

void frameEnd(const std::string& platform) {
  if (!gFile) return;
  ImGuiContext& g = *GImGui;
  const ImGuiIO& io = g.IO;
  bool down = false;
  for (bool d : io.MouseDown) down = down || d;
  if (down) gAfter = 20;
  const bool log = down || gAfter > 0 || !gLines.empty();
  if (gAfter > 0 && !down) --gAfter;
  // ImGui's own log of the frame (active item, focus, popups, viewports)
  std::string dbg;
  if (g.DebugLogBuf.size() < static_cast<int>(gDebugSeen)) gDebugSeen = 0;
  if (g.DebugLogBuf.size() > static_cast<int>(gDebugSeen)) dbg.assign(g.DebugLogBuf.c_str() + gDebugSeen, static_cast<size_t>(g.DebugLogBuf.size()) - gDebugSeen);
  if (g.DebugLogBuf.size() > 1 << 20) {
    g.DebugLogBuf.clear();
    g.DebugLogIndex.clear();
  }
  gDebugSeen = static_cast<size_t>(g.DebugLogBuf.size());
  if (!log && dbg.empty()) return;
  std::fprintf(gFile, "frame %d t %.3f dt %.4f mouse %.1f %.1f down %d %d clicked %d released %d active %08X (%s) alive %d hovered %08X (%s) nav \"%s\" moving \"%s\" focuslost %d %s\n",
               g.FrameCount, g.Time, static_cast<double>(io.DeltaTime), static_cast<double>(io.MousePos.x), static_cast<double>(io.MousePos.y), io.MouseDown[0] ? 1 : 0,
               io.MouseDown[1] ? 1 : 0, io.MouseClicked[0] ? 1 : 0, io.MouseReleased[0] ? 1 : 0, g.ActiveId, nameOf(g.ActiveIdWindow),
               g.ActiveIdIsAlive == g.ActiveId ? 1 : 0, g.HoveredId, nameOf(g.HoveredWindow), nameOf(g.NavWindow), nameOf(g.MovingWindow), io.AppFocusLost ? 1 : 0,
               platform.c_str());
  for (const std::string& l : gLines) std::fprintf(gFile, "  %s\n", l.c_str());
  gLines.clear();
  if (!dbg.empty()) {
    size_t s = 0;
    while (s < dbg.size()) {
      size_t e = dbg.find('\n', s);
      if (e == std::string::npos) e = dbg.size();
      std::fprintf(gFile, "  imgui: %s\n", dbg.substr(s, e - s).c_str());
      s = e + 1;
    }
  }
  std::fflush(gFile);
}

}  // namespace gl::draglog

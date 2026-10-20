// Gridline — pieces of the main window shared by the application and the
// plugin build (PH-09): the logo mark texture, the default docked workspace
// and keeping the docked panels usable while the window is resized.
#include "Shell.h"
#include "Widgets.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::shell {

// Tile mark coverage (guide §05 construction: 120u, r 28u, 10u bars at 40u/80u,
// inset 24u, true holes), supersampled. Returns size*size alpha values.
std::vector<unsigned char> markCoverage(int size) {
  auto inside = [](double x, double y) {
    const double U = 120, R = 28;
    double cx = std::clamp(x, R, U - R), cy = std::clamp(y, R, U - R);
    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > R * R) return false;
    for (double c : {40.0, 80.0}) {
      if (std::fabs(x - c) <= 5 && y >= 24 && y <= 96) return false;
      if (std::fabs(y - c) <= 5 && x >= 24 && x <= 96) return false;
    }
    return true;
  };
  std::vector<unsigned char> out(static_cast<size_t>(size) * static_cast<size_t>(size));
  const int ss = size < 64 ? 8 : 4;
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      int hit = 0;
      for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx) hit += inside((x + (sx + 0.5) / ss) * 120.0 / size, (y + (sy + 0.5) / ss) * 120.0 / size);
      out[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
          static_cast<unsigned char>((255 * hit + ss * ss / 2) / (ss * ss));
    }
  return out;
}

// Pixel-exact mark textures (white + coverage alpha), cached per size; tinted
// and faded by the draw list (theme::drawMark), so the holes are true holes.
ImTextureID markTexture(int px) {
  static std::map<int, GLuint> cache;
  if (px < 8 || px > 1024) return ImTextureID_Invalid;
  if (auto it = cache.find(px); it != cache.end()) return static_cast<ImTextureID>(it->second);
  auto cov = markCoverage(px);
  std::vector<unsigned char> rgba(cov.size() * 4);
  for (size_t i = 0; i < cov.size(); ++i) {
    rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = cov[i];
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px, px, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  cache[px] = tex;
  return static_cast<ImTextureID>(tex);
}

// FL-style default workspace (J-28, see core/Workspace.h): browser docked
// left, the mixer as a thin column docked right, the playlist in the centre
// (piano roll as a tab behind it) and the channel rack floating over the
// centre-left of the playlist. Sizes are picked in pixels for the window the
// layout is built in; the playlist node is the central node and absorbs later
// resizes. Returns the rack's floating rectangle (dock space coordinates).
workspace::Rect buildDefaultLayout(ImGuiID dockspace, ImVec2 size, int channels, const char* alsoLeft) {
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, size);
  const workspace::Layout L = workspace::defaultLayout(size.x, size.y, channels);
  ImGuiID left, rest, mixerNode, mainNode;
  ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, L.browserFrac, &left, &rest);
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, L.mixerFrac, &mixerNode, &mainNode);
  ImGui::DockBuilderDockWindow("Browser", left);
  ImGui::DockBuilderDockWindow("Undo history", left);
  if (alsoLeft) ImGui::DockBuilderDockWindow(alsoLeft, left);
  ImGui::DockBuilderDockWindow("Playlist", mainNode);
  ImGui::DockBuilderDockWindow("Piano roll", mainNode);
  ImGui::DockBuilderDockWindow("Mixer", mixerNode);
  ImGui::DockBuilderFinish(dockspace);
  return L.rack;
}

// Floating windows (the rack) stay reachable when the main window shrinks:
// their title bar and some of their width remain inside the dock space.
void keepFloatingInView(const char* name, ImVec2 pos, ImVec2 size) {
  ImGuiWindow* w = ImGui::FindWindowByName(name);
  if (!w || w->DockIsActive || !w->WasActive || w->Hidden) return;
  ImVec2 p = w->Pos;
  const float keep = std::min(w->Size.x, 160.0f);
  p.x = std::clamp(p.x, pos.x - (w->Size.x - keep), std::max(pos.x, pos.x + size.x - keep));
  p.y = std::clamp(p.y, pos.y, std::max(pos.y, pos.y + size.y - 40.0f));
  if (p.x != w->Pos.x || p.y != w->Pos.y) ImGui::SetWindowPos(w, p, ImGuiCond_Always);
}

// Keeps the docked workspace usable while the window is resized. ImGui keeps
// side panels at a fixed pixel size and gives all the change to the central
// node (playlist), so shrinking a large window could squeeze the playlist to
// nothing. Here side panels scale with the dock space and are clamped so the
// central area keeps at least half of every split.
namespace {
bool hostsWindow(const ImGuiDockNode* node, const char* name) {
  if (!node) return false;
  for (ImGuiWindow* w : node->Windows)
    if (w && std::strcmp(gl::ui::stableWindowName(w->Name), name) == 0) return true;  // J-61: translated titles
  return hostsWindow(node->ChildNodes[0], name) || hostsWindow(node->ChildNodes[1], name);
}
}  // namespace

void fitDockTree(ImGuiDockNode* node, ImVec2 scale, ImVec2 avail) {
  if (!node || node->IsLeafNode()) return;
  ImGuiDockNode* c0 = node->ChildNodes[0];
  ImGuiDockNode* c1 = node->ChildNodes[1];
  if (!c0 || !c1) return;
  const int axis = node->SplitAxis == ImGuiAxis_X ? 0 : 1;
  const float parent = axis == 0 ? avail.x : avail.y;
  ImGuiDockNode* side = c1->HasCentralNodeChild ? c0 : c0->HasCentralNodeChild ? c1 : nullptr;
  if (side && side->SizeRef[axis] > 0.0f) {
    float v = side->SizeRef[axis] * (axis == 0 ? scale.x : scale.y);
    const float minPx = axis == 0 ? 180.0f : 150.0f;
    float maxPx = std::max(parent * 0.5f, 1.0f);
    if (axis == 0 && hostsWindow(side, "Browser")) maxPx = std::min(maxPx, 340.0f);  // sidebar stays a sidebar
    v = std::clamp(v, std::min(minPx, parent * 0.5f), std::max(maxPx, std::min(minPx, parent * 0.5f)));
    side->SizeRef[axis] = std::floor(v);
  }
  ImVec2 a0 = avail, a1 = avail;
  if (side) {
    // The central branch gets what the side panel leaves.
    ImGuiDockNode* centre = side == c0 ? c1 : c0;
    ImVec2& ac = side == c0 ? a1 : a0;
    (axis == 0 ? ac.x : ac.y) = std::max(1.0f, parent - side->SizeRef[axis]);
    fitDockTree(centre, scale, ac);
    ImVec2& as = side == c0 ? a0 : a1;
    (axis == 0 ? as.x : as.y) = side->SizeRef[axis];
    fitDockTree(side, scale, as);
  } else {
    fitDockTree(c0, scale, a0);
    fitDockTree(c1, scale, a1);
  }
}


}  // namespace gl::shell

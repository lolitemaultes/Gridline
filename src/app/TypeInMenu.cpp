// Gridline - K-311: the type-in menu of every plugin control. See TypeInMenu.h.
#include "TypeInMenu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "App.h"
#include "Widgets.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::typein {

namespace {

bool tracking = false;
std::vector<Event> events;
int pressFrame = -1;
uint64_t revisionAtPress = 0;
std::vector<ParamId> menuParams;  // the open menu's parameters
std::optional<ParamId> typing;    // the parameter of the open type-in box
bool openType = false;
ImGuiID owner = 0;  // the window (editor) whose menu is open

void onItem(ImGuiID id, ImVec2 mn, ImVec2 mx) {
  if (!tracking || id == 0 || ImGui::FindWindowByID(id)) return;  // child windows are not controls
  Event e;
  e.item = true, e.id = id, e.x0 = mn.x, e.y0 = mn.y, e.x1 = mx.x, e.y1 = mx.y;
  events.push_back(e);
}

}  // namespace

void touch(const ParamId& pid) {
  if (!tracking) return;
  Event e;
  e.pid = pid;
  events.push_back(e);
}

void menuItems(App& a, const std::vector<ParamId>& params) {
  for (size_t i = 0; i < params.size(); ++i) {
    const auto d = describeParam(a.doc.song(), params[i]);
    if (!d) continue;
    ImGui::PushID(static_cast<int>(i) + 7311);
    if (params.size() > 1) ImGui::TextDisabled("%s", d->name.c_str());  // several: the same layout as the shared menu
    if (ImGui::MenuItem(i18n::tr("Type a value..."))) typing = params[i], openType = true, owner = 0;
    ImGui::PopID();
  }
}

void begin(App& a) {
  // Escape closes the menus this editor opened (keyboard navigation is off, so ImGui leaves them open).
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* root = g.CurrentWindow ? g.CurrentWindow->RootWindow : nullptr;
    for (int i = 0; root && i < g.OpenPopupStack.Size; ++i) {
      const ImGuiWindow* w = g.OpenPopupStack[i].Window;
      const ImGuiWindow* parent = w ? w->ParentWindow : nullptr;
      while (parent && (parent->Flags & ImGuiWindowFlags_Popup)) parent = parent->ParentWindow;
      if (parent && parent->RootWindow == root) {
        ImGui::ClosePopupToLevel(i, true);
        break;
      }
    }
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pressFrame != ImGui::GetFrameCount()) {
    pressFrame = ImGui::GetFrameCount();
    revisionAtPress = a.doc.revision();  // a right-click that edits (mute, delete a point) is not a menu
  }
  if (tracking || !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) return;
  tracking = true;
  events.clear();
  imhook::setTypeInHook(&onItem);
}

void end(App& a) {
  ImGui::PushID("##k311");
  if (tracking) {
    tracking = false;
    imhook::setTypeInHook(nullptr);
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 6.0f * 6.0f;  // a right-drag is a gesture
    if (std::getenv("GRIDLINE_TYPEIN_DEBUG"))
      std::printf("typein: release clicked %d edited %d popup %d hovered-window %d\n", clicked ? 1 : 0, a.doc.revision() != revisionAtPress ? 1 : 0,
                  ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ? 1 : 0,
                  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ? 1 : 0);
    if (clicked && a.doc.revision() == revisionAtPress && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      const ImGuiContext& g = *ImGui::GetCurrentContext();
      std::vector<ParamId> ps =
          paramsAt(events, io.MousePos.x, io.MousePos.y, io.MouseClickedPos[ImGuiMouseButton_Right].x, io.MouseClickedPos[ImGuiMouseButton_Right].y,
                   g.HoveredIdPreviousFrame ? g.HoveredIdPreviousFrame : g.HoveredId);
      if (std::getenv("GRIDLINE_TYPEIN_DEBUG")) {  // dev checks: what the release saw
        int items = 0;
        for (const Event& e : events) items += e.item ? 1 : 0;
        std::printf("typein: events %zu items %d hovered %08x found %zu\n", events.size(), items, static_cast<unsigned>(g.HoveredIdPreviousFrame), ps.size());
        std::fflush(stdout);
      }
      ps.erase(std::remove_if(ps.begin(), ps.end(), [&a](const ParamId& p) { return !describeParam(a.doc.song(), p) || !getParam(a.doc.song(), p); }),
               ps.end());
      if (!ps.empty()) {
        menuParams = std::move(ps);
        owner = ImGui::GetCurrentWindow()->ID;
        ImGui::OpenPopup("##menu");
      }
    }
    events.clear();
  }
  if (openType && owner == 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    owner = ImGui::GetCurrentWindow()->ID;  // chosen from a control's own menu (menuItems): the editor under the pointer
  if (owner != ImGui::GetCurrentWindow()->ID) {  // another editor's menu (or none)
    ImGui::PopID();
    return;
  }
  if (ImGui::BeginPopup("##menu")) {
    const Song& s = a.doc.song();
    for (size_t i = 0; i < menuParams.size(); ++i) {
      const ParamId pid = menuParams[i];
      const auto d = describeParam(s, pid);
      if (!d) continue;
      ImGui::PushID(static_cast<int>(i));
      if (i) ImGui::Separator();
      ImGui::TextDisabled("%s", d->name.c_str());
      if (menuParams.size() == 1) ImGui::Separator();
      double v = getParam(s, pid).value_or(d->def);
      ui::ParamResetMenuItem(pid, *d, v);
      if (ImGui::MenuItem(i18n::tr("Type a value..."))) typing = pid, openType = true;
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  if (openType) {
    ImGui::OpenPopup("##type");
    openType = false;
  }
  if (typing) {
    if (const auto d = describeParam(a.doc.song(), *typing)) ui::ParamTypePopup(*typing, *d);
    if (!ImGui::IsPopupOpen("##type")) typing.reset();
  }
  ImGui::PopID();
}

}  // namespace gl::typein

// Gridline — the parts of an installed plugin's window that do not depend on
// the window system (J-29 layout): the control bar and settings panel under
// the plugin's own interface, drawn with Dear ImGui, the bar's actions, and
// the routing that opens a plugin's own interface instead of Gridline's
// panel. Shared by the application (PluginWindow.cpp, SDL windows) and the
// plugin build (plugin/HostedWindows.cpp, native windows owned by the host's
// plugin window).
#pragma once
#include <cstdint>
#include <string>
#include "../host/EditorLayout.h"
#include "PluginUI.h"

namespace gl {

class App;
namespace host {
class HostedInstance;
}

namespace pluginbar {

enum class Action { None, Generic, Sandbox, Close, ChannelPanel };
inline constexpr int kNoPanelChange = -2;

struct Bar {
  App* app = nullptr;
  PluginTarget target;
  std::string type;
  host::layout::WindowLayout L;
  int panelPage = -1;               // -1: folded
  int wantPanel = kNoPanelChange;   // fold / unfold asked for by the bar (the window applies it)
  Action action = Action::None;     // button pressed (the window runs it after drawing)
};

// Draws the bar and, when unfolded, the settings panel as one ImGui window
// whose top-left corner is at (0, top) of the current frame.
void draw(Bar& b, host::HostedInstance* hi, float top);
// Background colour of the bar's GL surface (RGBA 0..1).
void background(float rgba[4]);
// Runs the action a bar asked for (may close the window that owns `b`).
void runAction(App& a, const PluginTarget& t, const std::string& type, Action act);

// The list of windows to show again once their new instance runs (sandbox
// or options switched): opens the ones that are ready.
void reopenPending(App& a);

}  // namespace pluginbar
}  // namespace gl

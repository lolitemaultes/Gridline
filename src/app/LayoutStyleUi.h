// Gridline - the layout style in the interface (K-112, core/LayoutStyle.h).
//
// Per frame, before ImGui::NewFrame, beginStyleFrame() puts the active layout's sizes into the
// ImGui style (field padding, item spacing, rounding, the gap between docked panels);
// endStyleFrame() after ImGui::Render restores them. Studio is a strict no-op: its style values
// are today's, the code returns at once, and Studio stays pixel-identical.
//
// Plugin editors keep their own look in every layout: drawPluginBody() runs inside a
// BaseStyleScope, which puts the base (Studio) sizes back while the editor draws.
//
// The style applies only to layouts built from their region shell (app/LayoutShellUi.h); the
// K-79 Live and Logic arrangements people use today keep Studio's style until their phases ship.
#pragma once
#include "../core/LayoutStyle.h"

namespace gl {
class App;
}

namespace gl::layoutui {

// The style in use (Studio's unless the active layout is built from its shell).
const layout::Style& style();
void beginStyleFrame(App& a, bool welcomeScreen);
void endStyleFrame();
// True between begin and end when a layout style is applied (not Studio).
bool styled();

struct BaseStyleScope {
  BaseStyleScope();
  ~BaseStyleScope();
  BaseStyleScope(const BaseStyleScope&) = delete;
  BaseStyleScope& operator=(const BaseStyleScope&) = delete;
  int pushed = 0;
  bool menuRight = false;
};

// K-112b (checkpoint 20): the base frame outline only, for a plugin window's own controls around the
// editor (its header, preset bar and tabs), which keep the look they have in every layout while the
// rest of the window follows the layout's sizes as before.
struct BaseBorderScope {
  BaseBorderScope();
  ~BaseBorderScope();
  BaseBorderScope(const BaseBorderScope&) = delete;
  BaseBorderScope& operator=(const BaseBorderScope&) = delete;
  bool pushed = false;
  bool menuRight = false;
};

// True while a BaseStyleScope is open (a plugin's own body is drawing: it keeps its look).
bool inBaseStyle();

// The dock area's inset from the bars and window edges (Studio: 4).
float dockMargin();
// The status bar's height: 0 in a layout without one (its hints go to the quick help box, its
// project and save state to the control bar); Studio and the others keep theirs.
float statusBarHeight();

// A section heading inside a menu: the labelled separator, or a plain one where the style has no
// menu headings (Live). menuTitles(): whether menus open with a title row.
void menuHeading(const char* text);
bool menuTitles();

}  // namespace gl::layoutui

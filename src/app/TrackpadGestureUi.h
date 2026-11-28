// Gridline — K-410: UI-thread bridge for TrackpadGesture (core/TrackpadGesture.h).
//
// Once per frame (after runDevInput and before ImGui::NewFrame()), beginFrame()
// syncs the "input.trackpadGestures" setting, drains the gesture event queue,
// and scales native OS points into ImGui logical coordinates. Each scrollable /
// zoomable canvas (Playlist, Piano Roll, Automation Editor, Loupe) consumes
// PinchZoom, SmartZoom, and Pan2D through the helpers below.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/TrackpadGesture.h"

namespace gl {
class App;
class Settings;
}

namespace gl::gestureui {

void beginFrame(const Settings& s, float uiScale);
void endFrame();

bool consumePinch(bool hovered, float x0, float y0, float x1, float y1, float& magnificationOut, ImVec2& posOut);
bool consumeSmartZoom(bool hovered, float x0, float y0, float x1, float y1, ImVec2& posOut);
bool consumePan2D(bool hovered, float x0, float y0, float x1, float y1, float& dxOut, float& dyOut);

// --dev-input commands:
//   pinch <mag> [x y]
//   smartzoom [x y]
//   pan <dx> <dy> [x y]
//   gestures
bool devCommand(App& app, const std::string& cmd, const std::vector<std::string>& args, int frame, ImVec2& pointer);

}  // namespace gl::gestureui

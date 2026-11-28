// Gridline — K-410: UI-thread trackpad gesture bridge (see TrackpadGestureUi.h).
#include "TrackpadGestureUi.h"
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include "../core/Settings.h"
#include "App.h"
#include "AutomationEditor.h"
#include "EditorZoomUi.h"

namespace gl::gestureui {

namespace {
gesture::GestureState gState;
}  // namespace

void beginFrame(const Settings& s, float uiScale) {
  const bool on = s.getBool("input.trackpadGestures");
  gesture::setEnabled(on);
  const auto events = gesture::take();
  gState.beginFrame(on, uiScale, events);
}

void endFrame() { gState.endFrame(); }

bool consumePinch(bool hovered, float x0, float y0, float x1, float y1, float& magnificationOut, ImVec2& posOut) {
  const float ez = edzoomui::currentZoom();
  const bool zoomedEditor = ez != 1.0f;
  float gx = 0.0f, gy = 0.0f;
  if (!gState.consumePinch(hovered, x0, y0, x1, y1, magnificationOut, gx, gy, zoomedEditor)) return false;
  posOut = zoomedEditor ? ImGui::GetIO().MousePos : ImVec2(gx, gy);
  return true;
}

bool consumeSmartZoom(bool hovered, float x0, float y0, float x1, float y1, ImVec2& posOut) {
  const float ez = edzoomui::currentZoom();
  const bool zoomedEditor = ez != 1.0f;
  float gx = 0.0f, gy = 0.0f;
  if (!gState.consumeSmartZoom(hovered, x0, y0, x1, y1, gx, gy, zoomedEditor)) return false;
  posOut = zoomedEditor ? ImGui::GetIO().MousePos : ImVec2(gx, gy);
  return true;
}

bool consumePan2D(bool hovered, float x0, float y0, float x1, float y1, float& dxOut, float& dyOut) {
  const float ez = edzoomui::currentZoom();
  const bool zoomedEditor = ez != 1.0f;
  if (!gState.consumePan2D(hovered, x0, y0, x1, y1, dxOut, dyOut, zoomedEditor)) return false;
  if (zoomedEditor && ez > 0.0f) {
    dxOut /= ez;
    dyOut /= ez;
  }
  return true;
}

bool devCommand(App& app, const std::string& cmd, const std::vector<std::string>& args, int frame, ImVec2& pointer) {
  ImGuiIO& io = ImGui::GetIO();
  if (cmd == "pinch" && !args.empty()) {
    const float mag = std::strtof(args[0].c_str(), nullptr);
    float x = pointer.x > -FLT_MAX * 0.5f ? pointer.x : io.MousePos.x;
    float y = pointer.y > -FLT_MAX * 0.5f ? pointer.y : io.MousePos.y;
    if (args.size() >= 3) {
      x = std::strtof(args[1].c_str(), nullptr);
      y = std::strtof(args[2].c_str(), nullptr);
      pointer = ImVec2(x, y);
      io.AddMousePosEvent(x, y);
    }
    gesture::TrackpadGesture g;
    g.kind = gesture::Kind::PinchZoom;
    g.phase = gesture::Phase::Changed;
    g.x = x;
    g.y = y;
    g.magnification = mag;
    g.precise = true;
    g.logical = true;
    gesture::post(g);
    return true;
  }
  if (cmd == "smartzoom") {
    float x = pointer.x > -FLT_MAX * 0.5f ? pointer.x : io.MousePos.x;
    float y = pointer.y > -FLT_MAX * 0.5f ? pointer.y : io.MousePos.y;
    if (args.size() >= 2) {
      x = std::strtof(args[0].c_str(), nullptr);
      y = std::strtof(args[1].c_str(), nullptr);
      pointer = ImVec2(x, y);
      io.AddMousePosEvent(x, y);
    }
    gesture::TrackpadGesture g;
    g.kind = gesture::Kind::SmartZoom;
    g.phase = gesture::Phase::Ended;
    g.x = x;
    g.y = y;
    g.precise = true;
    g.logical = true;
    gesture::post(g);
    return true;
  }
  if (cmd == "pan" && args.size() >= 2) {
    const float dx = std::strtof(args[0].c_str(), nullptr);
    const float dy = std::strtof(args[1].c_str(), nullptr);
    float x = pointer.x > -FLT_MAX * 0.5f ? pointer.x : io.MousePos.x;
    float y = pointer.y > -FLT_MAX * 0.5f ? pointer.y : io.MousePos.y;
    if (args.size() >= 4) {
      x = std::strtof(args[2].c_str(), nullptr);
      y = std::strtof(args[3].c_str(), nullptr);
      pointer = ImVec2(x, y);
      io.AddMousePosEvent(x, y);
    }
    gesture::TrackpadGesture g;
    g.kind = gesture::Kind::Pan2D;
    g.phase = gesture::Phase::Changed;
    g.x = x;
    g.y = y;
    g.dx = dx;
    g.dy = dy;
    g.precise = true;
    g.logical = true;
    gesture::post(g);
    return true;
  }
  if (cmd == "gestures") {
    const AutomationEditorView& av = automationEditorView();
    std::printf(
        "gestures frame %d: enabled %d playlist px %.7f sx %.3f sy %.3f th %.3f piano px %.7f sx %.3f sy %.3f rh %.3f auto sx %.3f px %.7f\n",
        frame, app.settings.getBool("input.trackpadGestures") ? 1 : 0, static_cast<double>(app.playlist.pxPerTick),
        static_cast<double>(app.playlist.scrollX), static_cast<double>(app.playlist.scrollY), static_cast<double>(app.playlist.trackHeight),
        static_cast<double>(app.piano.pxPerTick), static_cast<double>(app.piano.scrollX), static_cast<double>(app.piano.scrollY),
        static_cast<double>(app.piano.rowHeight), static_cast<double>(av.scrollX), static_cast<double>(av.pxPerTick));
    std::fflush(stdout);
    return true;
  }
  return false;
}

}  // namespace gl::gestureui

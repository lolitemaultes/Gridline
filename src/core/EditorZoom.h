// Gridline - K-19: the size of Gridline's own plugin editor windows.
//
// Every built-in editor is laid out in logical pixels at a fixed design size (the
// interface scale already multiplies those). On top of that each plugin window has its
// own zoom (75 % to 200 %): the editor is drawn at its design size and magnified (or
// reduced) as one picture (app/EditorZoomUi.h), so a fixed design keeps its proportions.
//
// K-136: every built-in editor opens at one global size (Settings > Interface, "Plugin
// window size", setting "ui.pluginWindowSize", kGlobalMin .. kGlobalMax, kGlobalDefault):
// at the default an editor's text and controls sit close to the main windows' density.
// A size picked for one plugin type overrides it (setting "ui.editorZoom":
// "gl.eq=1.5;gl.reverb=2"). With "ui.pluginWindowFit" on, a type without its own size opens
// at the automatic size when that is larger: the largest menu step at which the editor still
// takes no more than kAutoHeight of the work area's height and kAutoWidth of its width (so a
// large workspace opens small editors bigger). Whatever is asked for, the window never gets
// larger than the work area: the zoom is capped to what fits (and never below the smaller of
// the size asked for and 100 %, where the window scrolls as before).
// Pure functions (tested in tests/test_k19.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace gl::edzoom {

constexpr float kMin = 0.75f, kMax = 2.0f;
constexpr float kSteps[] = {0.75f, 0.85f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};  // the size menu
constexpr float kAutoHeight = 0.45f, kAutoWidth = 0.6f;
constexpr float kAutoSteps[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};  // the automatic size grows, never shrinks
// K-136: the global size of built-in editors (Settings > Interface), in 5 % steps.
constexpr float kGlobalMin = 0.75f, kGlobalMax = 1.5f, kGlobalDefault = 0.85f;

inline float globalSize(double setting) {
  if (!std::isfinite(setting) || setting <= 0) return kGlobalDefault;
  return std::clamp(static_cast<float>(std::round(setting * 20.0) / 20.0), kGlobalMin, kGlobalMax);
}

inline float clampZoom(float z) { return std::isfinite(z) ? std::clamp(z, kMin, kMax) : kMin; }
// Drag-resized zooms are kept to whole percent.
inline float roundZoom(float z) { return clampZoom(std::round(clampZoom(z) * 100.0f) / 100.0f); }

// The automatic zoom of an editor whose window is w x h logical px at 100 %, in a work area
// of aw x ah logical px.
inline float autoZoom(float w, float h, float aw, float ah) {
  if (!(w > 0) || !(h > 0) || !(aw > 0) || !(ah > 0)) return 1.0f;
  float best = 1.0f;
  for (float z : kAutoSteps)
    if (w * z <= aw * kAutoWidth + 0.5f && h * z <= ah * kAutoHeight + 0.5f) best = z;
  return best;
}

// `want` capped so a window of w x h at 100 % (zoomed as a whole) fits the work area;
// never under the smaller of `want` and 100 % (a window too large for the area scrolls).
inline float fitZoom(float want, float w, float h, float aw, float ah) {
  const float asked = clampZoom(want);
  float z = asked;
  if (w > 0 && aw > 0) z = std::min(z, aw / w);
  if (h > 0 && ah > 0) z = std::min(z, ah / h);
  return std::max(std::min(asked, 1.0f), std::floor(z * 100.0f + 1e-3f) / 100.0f);
}

// ---- the remembered zooms ("type=zoom;type=zoom")
inline std::map<std::string, float> parse(const std::string& s) {
  std::map<std::string, float> m;
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find(';', i);
    if (e == std::string::npos) e = s.size();
    const std::string item = s.substr(i, e - i);
    const size_t eq = item.rfind('=');
    if (eq != std::string::npos && eq > 0) {
      char* end = nullptr;
      const std::string v = item.substr(eq + 1);
      const double z = std::strtod(v.c_str(), &end);
      if (end != v.c_str() && std::isfinite(z) && z > 0) m[item.substr(0, eq)] = clampZoom(static_cast<float>(z));
    }
    i = e + 1;
  }
  return m;
}

inline std::string format(const std::map<std::string, float>& m) {
  std::string s;
  for (const auto& [type, z] : m) {
    if (type.empty() || type.find_first_of(";=") != std::string::npos) continue;
    char b[16];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(roundZoom(z)));
    if (!s.empty()) s += ';';
    s += type + "=" + b;
  }
  return s;
}

// The remembered zoom of `type`, 0 when none (automatic).
inline float zoomFor(const std::string& settings, const std::string& type) {
  const auto m = parse(settings);
  const auto it = m.find(type);
  return it == m.end() ? 0.0f : it->second;
}

// `settings` with `type` remembered at `z` (0 = automatic again: forgotten).
inline std::string withZoom(const std::string& settings, const std::string& type, float z) {
  auto m = parse(settings);
  if (z > 0) m[type] = roundZoom(z);
  else m.erase(type);
  return format(m);
}

// ---- the magnified picture: a point p of the editor drawn at `origin` with zoom z shows at
// origin + (p - origin) * z; the pointer at q on screen is over the editor's point
// origin + (q - origin) / z.
inline float toScreen(float origin, float p, float z) { return origin + (p - origin) * z; }
inline float toEditor(float origin, float q, float z) { return origin + (q - origin) / z; }

}  // namespace gl::edzoom

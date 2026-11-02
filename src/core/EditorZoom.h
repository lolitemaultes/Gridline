// Gridline - K-19: the size of Gridline's own plugin editor windows.
//
// Every built-in editor is laid out in logical pixels at a fixed design size (the
// interface scale already multiplies those). On top of that each plugin window has its
// own zoom (100 % to 200 %): the editor is drawn at its design size and magnified as one
// picture (app/EditorZoomUi.h), so a fixed design keeps its proportions.
//
// The zoom is remembered per plugin type (setting "ui.editorZoom": "gl.eq=1.5;gl.reverb=2").
// A type without a remembered zoom opens at the automatic size: the largest menu step at
// which the editor still takes no more than kAutoHeight of the work area's height and
// kAutoWidth of its width (so a large workspace opens small editors bigger), never under
// 100 %. Whatever is asked for, the window never gets larger than the work area: the zoom
// is capped to what fits (and never below 100 %, where the window scrolls as before).
// Pure functions (tested in tests/test_k19.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace gl::edzoom {

constexpr float kMin = 1.0f, kMax = 2.0f;
constexpr float kSteps[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};  // the size menu
constexpr float kAutoHeight = 0.45f, kAutoWidth = 0.6f;

inline float clampZoom(float z) { return std::isfinite(z) ? std::clamp(z, kMin, kMax) : kMin; }
// Drag-resized zooms are kept to whole percent.
inline float roundZoom(float z) { return clampZoom(std::round(clampZoom(z) * 100.0f) / 100.0f); }

// The automatic zoom of an editor whose window is w x h logical px at 100 %, in a work area
// of aw x ah logical px.
inline float autoZoom(float w, float h, float aw, float ah) {
  if (!(w > 0) || !(h > 0) || !(aw > 0) || !(ah > 0)) return kMin;
  float best = kMin;
  for (float z : kSteps)
    if (w * z <= aw * kAutoWidth + 0.5f && h * z <= ah * kAutoHeight + 0.5f) best = z;
  return best;
}

// `want` capped so a window of w x h at 100 % (zoomed as a whole) fits the work area;
// never under 100 %.
inline float fitZoom(float want, float w, float h, float aw, float ah) {
  float z = clampZoom(want);
  if (w > 0 && aw > 0) z = std::min(z, aw / w);
  if (h > 0 && ah > 0) z = std::min(z, ah / h);
  return std::max(kMin, std::floor(z * 100.0f) / 100.0f);
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

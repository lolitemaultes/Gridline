// Gridline - J-91: the interface scale the operating system asks for (see UiScale.h).
#include "UiScale.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace gl::uiscale {

namespace {

// A positive number from an environment value, 0 when unset or not a number.
double envNumber(const char* v) {
  if (!v || !*v) return 0;
  char* end = nullptr;
  const double d = std::strtod(v, &end);
  if (end == v || !std::isfinite(d) || d <= 0) return 0;
  return d;
}

float quarter(double s) { return static_cast<float>(std::clamp(std::round(s * 4.0) / 4.0, 1.0, 4.0)); }

enum class Source { Mac, WindowsDpi, Gdk, Qt, Compositor, Xft, Default };

Source decide(const Inputs& in, double& scale) {
  scale = 1.0;
  switch (in.desktop) {
    case Desktop::MacOS: return Source::Mac;
    case Desktop::Windows:
      // As before 0.6.1: SDL units are pixels (per-monitor DPI aware); the display's DPI is the scale.
      if (in.drawableW == in.windowW && in.displayDpi > 120) scale = in.displayDpi / 96.0;
      return Source::WindowsDpi;
    default: break;
  }
  const double gdk = envNumber(in.gdkScale), gdkDpi = envNumber(in.gdkDpiScale);
  if (gdk > 0 || gdkDpi > 0) {
    scale = (gdk > 0 ? std::floor(gdk + 0.5) : 1.0) * (gdkDpi > 0 ? gdkDpi : 1.0);
    return Source::Gdk;
  }
  if (const double qt = envNumber(in.qtScaleFactor); qt > 0) {
    scale = qt;
    return Source::Qt;
  }
  if (in.windowW > 0 && in.drawableW > in.windowW) return Source::Compositor;  // SDL already scales (Wayland)
  if (in.xftDpi > 0) {
    scale = in.xftDpi / 96.0;
    return Source::Xft;
  }
  return Source::Default;
}

float gDetected = 1.0f;

}  // namespace

float systemScale(const Inputs& in) {
  double s = 1.0;
  decide(in, s);
  return quarter(s);
}

std::string systemScaleSource(const Inputs& in) {
  double s = 1.0;
  char buf[96];
  switch (decide(in, s)) {
    case Source::Mac: return "macOS backing scale";
    case Source::WindowsDpi: std::snprintf(buf, sizeof buf, "display %.0f dpi", static_cast<double>(in.displayDpi)); return buf;
    case Source::Gdk: return "GDK_SCALE / GDK_DPI_SCALE";
    case Source::Qt: return "QT_SCALE_FACTOR";
    case Source::Compositor: return "compositor (window pixel density)";
    case Source::Xft: std::snprintf(buf, sizeof buf, "Xft.dpi %.0f", static_cast<double>(in.xftDpi)); return buf;
    case Source::Default: break;
  }
  return "default";
}

namespace {
// A number resource in an X resource database string (lines "name:\tvalue"; any of
// `names`, the binding written "." or "*"), 0 when absent.
double parseResource(const char* res, std::initializer_list<const char*> names) {
  if (!res) return 0;
  const char* p = res;
  while (*p) {
    const char* eol = std::strchr(p, '\n');
    const size_t len = eol ? static_cast<size_t>(eol - p) : std::strlen(p);
    std::string line(p, len);
    p += len + (eol ? 1 : 0);
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    line.erase(0, b);
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    bool match = false;
    for (const char* n : names) match = match || name == n;
    if (!match) continue;
    const char* v = line.c_str() + colon + 1;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end != v && std::isfinite(d) && d > 0) return d;
  }
  return 0;
}
}  // namespace

float parseXftDpi(const char* res) { return static_cast<float>(parseResource(res, {"Xft.dpi", "Xft*dpi", "*dpi"})); }

int parseXcursorSize(const char* res) {
  const double d = parseResource(res, {"Xcursor.size", "Xcursor*size", "*cursorSize"});
  return d >= 8 && d <= 512 ? static_cast<int>(d + 0.5) : 0;
}

double pointerArrowHeight(PointerSystem sys, double nominal, double multiplier) {
  if (!(nominal > 0)) {
    switch (sys) {
      case PointerSystem::MacOS: nominal = 23; break;    // the arrow image, points
      case PointerSystem::Windows: nominal = 32; break;  // SM_CYCURSOR at 100 %
      case PointerSystem::Unix: nominal = 24; break;     // Xcursor / GNOME default
    }
  }
  const double m = multiplier > 0 ? std::clamp(multiplier, 1.0, 4.0) : 1.0;
  double fill = 0.8;  // Adwaita / Yaru / DMZ: the arrow spans about 4/5 of the nominal size
  if (sys == PointerSystem::MacOS) fill = 0.87;  // the image has room for the shadow below
  if (sys == PointerSystem::Windows) fill = 0.62;  // the 32 px cell: a 19-20 px arrow
  return nominal * fill * m;
}

float effectiveScale(double userScale, bool followSystem, float system, int winW, int winH) {
  float s = static_cast<float>(std::clamp(userScale, 0.75, 2.0)) * (followSystem ? std::max(1.0f, system) : 1.0f);
  if (s > 1.0f && winW > 0 && winH > 0)  // never shrink the logical workspace below the minimum layout size
    s = std::max(1.0f, std::min({s, static_cast<float>(winW) / 960.0f, static_cast<float>(winH) / 600.0f}));
  return s;
}

void setDetected(float s) { gDetected = std::max(1.0f, s); }
float detected() { return gDetected; }

}  // namespace gl::uiscale

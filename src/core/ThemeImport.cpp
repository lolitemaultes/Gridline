// Gridline - K-104: theme file import (see ThemeImport.h).
#include "ThemeImport.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "Settings.h"

namespace gl::themeimport {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool number(const std::string& v, long long& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  out = std::strtoll(v.c_str(), &end, 10);
  return end && *end == '\0';
}

std::string hexText(uint32_t rgb) {
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", rgb & 0xFFFFFFu);
  return b;
}

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

}  // namespace

uint32_t rgbOf(long long value) {
  const uint32_t v = static_cast<uint32_t>(value) & 0xFFFFFFu;  // the top byte (set when negative) is not a colour
  return ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
}

bool parse(const std::string& text, File& out, std::string* err) {
  out = File{};
  std::string t = text;
  if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
      static_cast<unsigned char>(t[2]) == 0xBF)
    t.erase(0, 3);
  std::istringstream in(t);
  std::string line;
  int known = 0;
  while (std::getline(in, line)) {
    line = trim(line);
    const size_t eq = line.find('=');
    if (line.empty() || eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    long long n = 0;
    const bool isNum = number(val, n);
    auto knob = [&](std::optional<int>& f) {
      if (isNum) f = static_cast<int>(std::clamp<long long>(n, -256, 256)), ++known;
    };
    auto colour = [&](std::optional<uint32_t>& f) {
      if (isNum) f = rgbOf(n), ++known;
    };
    if (key == "hue") knob(out.hue);
    else if (key == "saturation") knob(out.saturation);
    else if (key == "lightness") knob(out.lightness);
    else if (key == "contrast") knob(out.contrast);
    else if (key == "text") knob(out.text);
    else if (key == "selected") colour(out.selected);
    else if (key == "highlight") colour(out.highlight);
    else if (key == "mute") colour(out.mute);
    else if (key == "stepeven") colour(out.stepEven);
    else if (key == "stepodd") colour(out.stepOdd);
    else out.skipped.push_back(trim(line.substr(0, eq)));
  }
  if (known == 0) {
    if (err) *err = "No theme settings found in the file";
    return false;
  }
  return true;
}

void toSettings(const File& f, Settings& s) {
  // Lightness: past a quarter of the way up the panels are light; the rest of the way is brightness.
  const int light = f.lightness.value_or(0);
  const bool lightMode = light > 64;
  s.set("theme.mode", lightMode ? "light" : "dark");
  s.set("ui.darkTheme", !lightMode);
  const double bright = lightMode ? clampd((light - 152) / 256.0, -0.5, 0.5) : clampd(light / 256.0, -0.95, 0.6);
  s.set("theme.brightness", bright);
  s.set("theme.contrast", clampd(f.contrast.value_or(0) / 128.0, -1.0, 1.0));
  s.set("theme.hue", clampd(f.hue.value_or(0) / 256.0, -1.0, 1.0));
  s.set("theme.saturation", clampd(f.saturation.value_or(0) / 256.0, -1.0, 1.0));
  s.set("theme.whiteText", !lightMode && f.text.value_or(0) > 128);
  s.set("theme.accent", f.selected ? hexText(*f.selected) : std::string());
  s.set("theme.hot", f.highlight ? hexText(*f.highlight) : std::string());
  s.set("theme.led", f.mute ? hexText(*f.mute) : std::string());
  s.set("theme.playhead", std::string());
  s.set("theme.stepEven", f.stepEven ? hexText(*f.stepEven) : std::string());
  s.set("theme.stepOdd", f.stepOdd ? hexText(*f.stepOdd) : std::string());
}

bool importFile(const std::string& path, Settings& s, std::string* err, File* parsed) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "Cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (ss.str().size() > (1u << 20)) {
    if (err) *err = "Not a theme file (too large)";
    return false;
  }
  File f;
  if (!parse(ss.str(), f, err)) return false;
  toSettings(f, s);
  if (parsed) *parsed = f;
  return true;
}

}  // namespace gl::themeimport

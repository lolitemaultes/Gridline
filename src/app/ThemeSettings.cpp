// Gridline - theme choice in the settings (see ThemeSettings.h).
#include "ThemeSettings.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include "../core/Settings.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gl::theme {

namespace {
#if !defined(_WIN32)
// Output of a short shell query, lower case ("" when it fails).
std::string query(const char* cmd) {
  std::string out;
  if (FILE* f = popen(cmd, "r")) {
    char buf[256];
    while (std::fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
  }
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
#endif
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
}  // namespace

std::string colourText(uint32_t rgb) {
  if (rgb == Tweaks::kKeep) return {};
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", rgb & 0xFFFFFFu);
  return b;
}

uint32_t parseColour(const std::string& text) {
  std::string t = text;
  if (!t.empty() && t[0] == '#') t = t.substr(1);
  if (t.size() != 6) return Tweaks::kKeep;
  for (char c : t)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return Tweaks::kKeep;
  return static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
}

Mode modeFromSettings(const Settings& s) {
  const std::string m = s.getString("theme.mode");
  if (m == "system") return Mode::System;
  if (m == "light") return Mode::Light;
  if (m == "dark") return Mode::Dark;
  return s.getBool("ui.darkTheme") ? Mode::Dark : Mode::Light;
}

void modeToSettings(Mode m, Settings& s) {
  s.set("theme.mode", m == Mode::System ? "system" : m == Mode::Light ? "light" : "dark");
  if (m != Mode::System) s.set("ui.darkTheme", m == Mode::Dark);
}

Tweaks tweaksFromSettings(const Settings& s) {
  Tweaks t;
  t.accent = parseColour(s.getString("theme.accent"));
  t.hot = parseColour(s.getString("theme.hot"));
  t.led = parseColour(s.getString("theme.led"));
  t.playhead = parseColour(s.getString("theme.playhead"));
  t.contrast = static_cast<float>(std::clamp(s.getNumber("theme.contrast"), -1.0, 1.0));
  t.brightness = static_cast<float>(std::clamp(s.getNumber("theme.brightness"), -1.0, 1.0));
  return t;
}

void tweaksToSettings(const Tweaks& t, Settings& s) {
  s.set("theme.accent", colourText(t.accent));
  s.set("theme.hot", colourText(t.hot));
  s.set("theme.led", colourText(t.led));
  s.set("theme.playhead", colourText(t.playhead));
  s.set("theme.contrast", static_cast<double>(t.contrast));
  s.set("theme.brightness", static_cast<double>(t.brightness));
}

void resetThemeSettings(Settings& s) { tweaksToSettings(Tweaks{}, s); }

bool systemPrefersDark() {
#if defined(_WIN32)
  // Settings > Personalisation > Colours > "Choose your app mode" (absent before Windows 10 1809: light).
  DWORD light = 1, size = sizeof light;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                   RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
    return false;
  return light == 0;
#elif defined(__APPLE__)
  // Light mode has no AppleInterfaceStyle key at all.
  return has(query("defaults read -g AppleInterfaceStyle 2>/dev/null"), "dark");
#else
  if (const char* g = std::getenv("GTK_THEME"); g && *g) {
    std::string t = g;
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return has(t, "dark");
  }
  const std::string scheme = query("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
  if (has(scheme, "prefer-dark")) return true;
  if (has(scheme, "prefer-light")) return false;
  const std::string gtk = query("gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null");
  return gtk.empty() || has(gtk, "dark");
#endif
}

void applyFromSettings(const Settings& s, bool requerySystem) {
  static int systemDark = -1;  // cached desktop answer (-1 = not asked yet)
  const Mode m = modeFromSettings(s);
  if (m == Mode::System && (requerySystem || systemDark < 0)) systemDark = systemPrefersDark() ? 1 : 0;
  const bool dark = m == Mode::System ? systemDark != 0 : m == Mode::Dark;
  const Tweaks t = tweaksFromSettings(s);
  if (t != tweaks()) setTweaks(t);  // re-applies the current base
  apply(dark);
}

}  // namespace gl::theme

// Gridline - control style (J-129, Settings > Theme > Controls): how knobs,
// faders, buttons, meters, fields and displays are drawn, everywhere (the main
// window, every plugin editor, the Gridline Suite). A style changes drawing
// only, never sizes or hit boxes. Setting: theme.controls = "slate" (default) |
// "graphite" | "hairline" | "lumen" | "classic"; anything else reads as slate.
//
// The drawing itself is in Look*.cpp (gl::look); this header only holds the
// choice so that settings code and tests need nothing else.
#pragma once
#include <cstring>
#include <string>

namespace gl::ui {

enum class ControlStyle : int {
  Classic = 0,  // the look before J-129
  Graphite,     // machined graphite and recessed Paper light (the app icon's material)
  Hairline,     // flat, hairlines and type, one accent
  Lumen,        // dark glass, values drawn as light
  Slate,        // the studio slate roles with one consistent finish (default)
  Count
};
constexpr ControlStyle kDefaultControlStyle = ControlStyle::Slate;

// Setting value ("slate", ...).
inline const char* controlStyleName(ControlStyle s) {
  static const char* const names[] = {"classic", "graphite", "hairline", "lumen", "slate"};
  const int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(ControlStyle::Count) ? names[i] : names[static_cast<int>(kDefaultControlStyle)];
}
// Strict parse (false for an unknown name).
inline bool parseControlStyle(const char* s, ControlStyle& out) {
  if (!s) return false;
  for (int i = 0; i < static_cast<int>(ControlStyle::Count); ++i)
    if (std::strcmp(s, controlStyleName(static_cast<ControlStyle>(i))) == 0) {
      out = static_cast<ControlStyle>(i);
      return true;
    }
  return false;
}
// Setting text to a style: unknown or empty = the default (slate).
inline ControlStyle controlStyleFromText(const std::string& s) {
  ControlStyle st = kDefaultControlStyle;
  return parseControlStyle(s.c_str(), st) ? st : kDefaultControlStyle;
}

namespace detail {
inline ControlStyle gControlStyle = kDefaultControlStyle;  // the user's choice
inline int gForcedStyle = -1;                              // dev --look (wins over the setting)
inline int gScopedStyle = -1;                              // ControlStyleScope (gallery, previews)
}  // namespace detail

// The style everything draws with now.
inline ControlStyle controlStyle() {
  if (detail::gScopedStyle >= 0) return static_cast<ControlStyle>(detail::gScopedStyle);
  if (detail::gForcedStyle >= 0) return static_cast<ControlStyle>(detail::gForcedStyle);
  return detail::gControlStyle;
}
inline void setControlStyle(ControlStyle s) { detail::gControlStyle = s; }
inline void forceControlStyle(ControlStyle s) { detail::gForcedStyle = static_cast<int>(s); }
// True when the shared drawers hand drawing to the style layer (every style but Classic).
inline bool styledControls() { return controlStyle() != ControlStyle::Classic; }

// Draws with another style for a while (style previews, the dev gallery).
struct ControlStyleScope {
  explicit ControlStyleScope(ControlStyle s) : prev(detail::gScopedStyle) { detail::gScopedStyle = static_cast<int>(s); }
  ~ControlStyleScope() { detail::gScopedStyle = prev; }
  ControlStyleScope(const ControlStyleScope&) = delete;
  ControlStyleScope& operator=(const ControlStyleScope&) = delete;
  int prev;
};

}  // namespace gl::ui

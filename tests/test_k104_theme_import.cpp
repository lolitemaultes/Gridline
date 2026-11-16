// Gridline - K-104: theme files of the reference DAW onto Gridline's theme settings
// (core/ThemeImport.h): parsing (byte order mark, CRLF, case, junk lines), the colour order
// (0x00BBGGRR, negative values with the top byte set), the mapping onto theme.* keys, and
// that importing resets what the file leaves out.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "../src/core/Settings.h"
#include "../src/core/ThemeImport.h"

using namespace gl;
namespace ti = gl::themeimport;

int main() {
  expect(ti::rgbOf(0x0044AAFF) == 0xFFAA44u, "colour: 0x00BBGGRR becomes 0xRRGGBB");
  expect(ti::rgbOf(-1) == 0xFFFFFFu && ti::rgbOf(static_cast<long long>(static_cast<int32_t>(0xFF102030u))) == 0x302010u,
         "colour: negative values keep their low three bytes");

  // A made-up dark theme (written for this test).
  const std::string dark = "\xEF\xBB\xBFHue=64\r\nSaturation=-128\r\nLightness=-128\r\nContrast=32\r\nText=200\r\n"
                           "Selected=4500000\r\nHighlight=-1000000\r\nMute=65280\r\nStepEven=3355443\r\nStepOdd=-11184811\r\n"
                           "Meter0=123\r\nBackPicFilename=pic.png\r\n\r\nnot a setting\r\n";
  ti::File f;
  expect(ti::parse(dark, f), "parse: a theme file with a byte order mark and CRLF");
  expect(f.hue == 64 && f.saturation == -128 && f.lightness == -128 && f.contrast == 32 && f.text == 200, "parse: the tint knobs");
  expect(f.selected == ti::rgbOf(4500000) && f.highlight == ti::rgbOf(-1000000) && f.mute == 0x00FF00u && f.stepEven == ti::rgbOf(3355443) &&
             f.stepOdd == ti::rgbOf(-11184811),
         "parse: the role colours");
  expect(f.skipped.size() == 2, "parse: keys without a role are counted as skipped");

  Settings s;
  s.set("theme.playhead", "#123456");
  ti::toSettings(f, s);
  expect(s.getString("theme.mode") == "dark" && s.getBool("ui.darkTheme"), "map: low lightness is the dark mode");
  expect(std::fabs(s.getNumber("theme.brightness") + 0.5) < 1e-9, "map: lightness -128 is brightness -50 %");
  expect(std::fabs(s.getNumber("theme.contrast") - 0.25) < 1e-9, "map: contrast 32 is +25 %");
  expect(std::fabs(s.getNumber("theme.hue") - 0.25) < 1e-9 && std::fabs(s.getNumber("theme.saturation") + 0.5) < 1e-9, "map: hue and saturation");
  expect(s.getBool("theme.whiteText"), "map: raised text on dark panels is white text");
  char want[16];
  std::snprintf(want, sizeof want, "#%06X", ti::rgbOf(4500000));
  expect(s.getString("theme.accent") == want, "map: selected is the accent");
  expect(s.getString("theme.led") == "#00FF00", "map: mute is the LEDs");
  expect(s.getString("theme.playhead").empty(), "map: what the file leaves out goes back to the theme's");

  ti::File l;
  expect(ti::parse("lightness=160\ncontrast=-300\n", l), "parse: lower case keys, LF lines");
  Settings s2;
  ti::toSettings(l, s2);
  expect(s2.getString("theme.mode") == "light" && !s2.getBool("ui.darkTheme"), "map: high lightness is the light mode");
  expect(std::fabs(s2.getNumber("theme.contrast") + 1.0) < 1e-9, "map: knobs are clamped");
  expect(s2.getString("theme.accent").empty() && s2.getString("theme.stepEven").empty(), "map: no colours, theme colours");

  ti::File none;
  std::string err;
  expect(!ti::parse("hello\nworld=1\n", none, &err) && !err.empty(), "parse: a file with no theme settings is refused");

  const std::string path = (std::filesystem::temp_directory_path() / "gl_k104_theme.flstheme").string();
  {
    std::ofstream o(path, std::ios::binary);
    o << dark;
  }
  Settings s3;
  expect(ti::importFile(path, s3, &err) && s3.getString("theme.mode") == "dark", "import: reads a file from disk");
  expect(!ti::importFile(path + ".missing", s3, &err), "import: a missing file is refused");
  std::filesystem::remove(path);
  return finish();
}

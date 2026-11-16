// Gridline - K-104: reads a theme file of the reference DAW (a small "key=value" text file: the
// tint adjustments and a few role colours) and puts it onto Gridline's own theme settings
// (theme.* keys, ThemeSettings.h). Only the user's own file is read; nothing of it is kept but the
// numbers. Colours in the file are 0x00BBGGRR integers (negative: the same with the top byte set).
//
// What maps where:
//   Lightness  -> dark or light mode and the brightness of the panels
//   Contrast   -> contrast;  Hue / Saturation -> the panels' tint (theme.hue, theme.saturation)
//   Text       -> white text on dark panels when strongly raised
//   Selected   -> accent;  Highlight -> lit glyphs;  Mute -> LEDs
//   StepEven / StepOdd -> the step sequencer's two off colours (theme.stepEven, theme.stepOdd)
// Keys Gridline has no role for (meters, grid backgrounds, background pictures) are counted in
// `skipped` so the caller can say what stayed out.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gl {
class Settings;
}

namespace gl::themeimport {

struct File {
  std::optional<int> hue, saturation, lightness, contrast, text;  // the tint knobs, -256 .. 256
  std::optional<uint32_t> selected, highlight, mute, stepEven, stepOdd;  // 0xRRGGBB
  std::vector<std::string> skipped;  // keys read but not used
};

// 0x00BBGGRR (or its negative form) -> 0xRRGGBB.
uint32_t rgbOf(long long value);
// Parses the text of a theme file (UTF-8, an optional byte order mark, CRLF or LF). False when no
// known key is found (not a theme file).
bool parse(const std::string& text, File& out, std::string* err = nullptr);
// Writes the theme settings the file describes (mode, tweaks); keys the file leaves out go back
// to their defaults, so importing a file always gives the same look.
void toSettings(const File& f, Settings& s);
// Reads and applies a file in one go (false with a message when it cannot be read or parsed).
bool importFile(const std::string& path, Settings& s, std::string* err = nullptr, File* parsed = nullptr);

}  // namespace gl::themeimport

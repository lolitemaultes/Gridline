// Gridline — "synths" pack, J-33 group "pixel": parameter layout of
//   gl.pixeltone "Pixel Tone"  plays a picture: every row is a sine partial
//                (red = left level, green = right level, blue optionally bends
//                its pitch), the columns are read left to right at a set
//                speed; log / linear / harmonic frequency layouts, grainy and
//                widen modes, looping.
// Parameters 0..6 are the reference instrument's control tags (integer values
// in the reference's ranges); 7 is its saved maximum picture height.
// PluginState::data = the picture: "P <width> <height>" then one line per row
// (top row first) of width × RRGGBB hex.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::packs::synths::pixel {

enum PixelParam { PxRange, PxLength, PxScale, PxUseBlue, PxGrainy, PxLoop, PxWiden, PxMaxHeight, PxCount };
enum PixelScale { ScLog, ScLinear, ScHarmonics };
inline constexpr int kMaxRows = 999;

struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;  // h rows × w pixels × (r, g, b), top row first
};
std::string formatPicture(const Picture& p);
bool parsePicture(const std::string& text, Picture& p);
// Samples per column at 44.1 kHz for a length value 0..4096.
int columnSamples44(int length);

}  // namespace gl::packs::synths::pixel

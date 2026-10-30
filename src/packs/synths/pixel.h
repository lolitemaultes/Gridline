// Gridline — "synths" pack, J-33 group "pixel": parameter layout of
//   gl.pixeltone "Pixel Tone"  plays a picture: every row is a sine partial
//                (red = left level, green = right level, blue optionally bends
//                its pitch), the columns are read left to right at a set
//                speed; log / linear / harmonic frequency layouts, grainy and
//                widen modes, looping.
// Parameters 0..6 are the reference instrument's control tags (integer values
// in the reference's ranges); 7 is its saved maximum picture height.
// PluginState::data = the picture: "P <width> <height>" then one line per row
// (top row first) of width × RRGGBB hex; J-184: a loaded picture adds the
// line "N <source width> <source height> <file name>" after the rows (older
// versions read the rows only and ignore it).
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
  // J-184: where a loaded picture came from (empty / 0 for the built-in one).
  std::string name;
  int srcW = 0, srcH = 0;
};
std::string formatPicture(const Picture& p);
bool parsePicture(const std::string& text, Picture& p);

// ---- J-184: pictures from image files (UI thread or a worker, never audio).
// A picture is reduced to at most `maxRows` rows (the "Max rows" parameter),
// keeping its aspect ratio, and to at most kMaxColumns columns and
// kMaxPixels pixels in all (so a large photo stays fast to play and the
// project small: 262144 pixels are about 1.5 MB of picture text); smaller
// pictures are kept as they are. Each new pixel averages the source pixels it
// covers; transparent pixels are laid onto black (silence).
inline constexpr int kMaxColumns = 1024;
inline constexpr int kMaxPixels = 262144;
inline constexpr int kMaxSourcePixels = 120000000;      // larger files are refused (memory)
inline constexpr size_t kMaxFileBytes = 256u << 20;    // 256 MB
// The size a w × h source becomes.
void pictureSize(int w, int h, int maxRows, int& outW, int& outH);
bool pictureFromRgba(const uint8_t* rgba, int w, int h, int maxRows, Picture& out);
// Encoded bytes (PNG, JPEG, BMP, GIF first frame, TGA, PSD, HDR) to a picture;
// false with a short reason in `err` for files that are not pictures.
bool decodePicture(const std::string& bytes, int maxRows, Picture& out, std::string* err = nullptr);
// A file; `out.name` is its file name.
bool loadPictureFile(const std::string& path, int maxRows, Picture& out, std::string* err = nullptr);
// Extensions loadPictureFile reads (lower case, with the dot) and a check by name.
const std::vector<std::string>& pictureExtensions();
bool isPictureFile(const std::string& path);
// Small edits: mirror left / right (time reversed), upside down (pitch
// mirrored), colour negative. They keep the name and source size.
void flipColumns(Picture& p);
void flipRows(Picture& p);
void invertColours(Picture& p);
// Samples per column at 44.1 kHz for a length value 0..4096.
int columnSamples44(int length);

}  // namespace gl::packs::synths::pixel

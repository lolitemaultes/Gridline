// Gridline — "synths" pack, J-33 group "pixel": catalogue entry, factory
// presets and the picture text of Pixel Tone (gl.pixeltone).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "../Packs.h"
#include "pixel.h"

namespace gl::packs::synths {

namespace pixel {

std::string formatPicture(const Picture& p) {
  std::string s = "P " + std::to_string(p.w) + " " + std::to_string(p.h) + "\n";
  static const char* hx = "0123456789ABCDEF";
  for (int y = 0; y < p.h; ++y) {
    for (int x = 0; x < p.w; ++x)
      for (int c = 0; c < 3; ++c) {
        const uint8_t v = p.rgb[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)];
        s += hx[v >> 4];
        s += hx[v & 15];
      }
    s += "\n";
  }
  return s;
}

bool parsePicture(const std::string& text, Picture& p) {
  p = Picture{};
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.size() < 2 || line[0] != 'P') return false;
  int w = 0, h = 0;
  if (std::sscanf(line.c_str() + 1, "%d %d", &w, &h) != 2 || w < 1 || h < 1 || w > 100000 || h > kMaxRows) return false;
  p.w = w, p.h = h;
  p.rgb.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
  auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0)); };
  for (int y = 0; y < h && std::getline(in, line); ++y)
    for (size_t i = 0; i + 1 < line.size() && i / 2 < static_cast<size_t>(w) * 3; i += 2)
      p.rgb[static_cast<size_t>(y) * static_cast<size_t>(w) * 3 + i / 2] = static_cast<uint8_t>(nib(line[i]) * 16 + nib(line[i + 1]));
  return true;
}

int columnSamples44(int length) {
  const float x = static_cast<float>(length) / 4096.f;
  return static_cast<int>(std::lround(256.f * (std::pow(33.f, x) - 1.f))) + 64;
}

}  // namespace pixel

namespace {

using namespace pixel;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }

// A small original picture: rising and falling diagonal strokes.
std::string defaultPicture() {
  Picture p;
  p.w = 48, p.h = 16;
  p.rgb.assign(static_cast<size_t>(p.w * p.h * 3), 0);
  for (int x = 0; x < p.w; ++x) {
    const int y1 = 15 - (x % 16), y2 = x % 16;
    for (int y : {y1, y2}) {
      uint8_t* px = &p.rgb[static_cast<size_t>((y * p.w + x) * 3)];
      px[0] = static_cast<uint8_t>(y == y1 ? 255 : 120), px[1] = static_cast<uint8_t>(y == y2 ? 255 : 120), px[2] = 0;
    }
  }
  return formatPicture(p);
}

}  // namespace

PluginInfo pixelToneInfo() {
  std::vector<ParamDesc> q = {lin("Frequency span", "", 24, 9600, 7200),
                              lin("Column length", "", 0, 4096, 2048),
                              ParamDesc{"Scale", "", 0, 2, 0, ParamKind::Choice, ParamScale::Linear, {"Log", "Linear", "Harmonics"}},
                              tog("Blue bends pitch", false),
                              tog("Grainy", false),
                              tog("Loop", true),
                              tog("Widen", false),
                              lin("Max rows", "", 3, 999, 23)};
  std::vector<PluginInfo::Preset> presets = {
      {"Sweep", {}},
      {"Slow Harmonics", {{PxScale, 2.f}, {PxLength, 3000.f}, {PxRange, 4800.f}}},
      {"Grain Wash", {{PxGrainy, 1.f}, {PxWiden, 1.f}, {PxLength, 2600.f}}},
  };
  PluginInfo p{"gl.pixeltone", "Pixel Tone", "Synth", PluginKind::Generator, std::move(q), false, std::move(presets)};
  p.editorWidth = 620;
  return p;
}

std::string pixelToneDefaultData() { return defaultPicture(); }

}  // namespace gl::packs::synths

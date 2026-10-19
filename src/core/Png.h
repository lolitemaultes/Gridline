// Gridline — minimal PNG codec for the app's own cached images (plugin picker
// thumbnails). Writes 8-bit RGB / RGBA (Sub filter, fixed-Huffman deflate);
// reads 8-bit RGB / RGBA, non-interlaced, any filter.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::png {

struct Image {
  int width = 0, height = 0;
  int channels = 0;           // 3 = RGB, 4 = RGBA
  std::vector<uint8_t> pixels;  // rows top to bottom, width * channels bytes each
};

// zlib stream of `data` (fixed Huffman codes with LZ77 matching).
std::vector<uint8_t> deflateZlib(const uint8_t* data, size_t n);

std::vector<uint8_t> encode(const Image& img);
bool decode(const uint8_t* data, size_t n, Image& out);

bool writeFile(const std::string& path, const Image& img);
bool readFile(const std::string& path, Image& out);

// Box-filtered downscale (RGB / RGBA) to w x h.
Image resize(const Image& src, int w, int h);

}  // namespace gl::png

// Gridline — "synths" pack, J-33 group "pixel": catalogue entry, factory
// presets and the picture text of Pixel Tone (gl.pixeltone).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../../../third_party/stb/gridline_stb_pictures.h"
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
  if (!p.name.empty()) {  // J-184: the source line
    std::string n = p.name.substr(0, 200);
    for (char& c : n)
      if (c == '\n' || c == '\r') c = ' ';
    s += "N " + std::to_string(p.srcW) + " " + std::to_string(p.srcH) + " " + n + "\n";
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
  while (std::getline(in, line)) {  // J-184: "N <source w> <source h> <name>"
    if (line.size() < 2 || line[0] != 'N' || line[1] != ' ') continue;
    int sw = 0, sh = 0, used = 0;
    if (std::sscanf(line.c_str() + 2, "%d %d %n", &sw, &sh, &used) < 2) continue;
    p.srcW = std::max(0, sw), p.srcH = std::max(0, sh);
    p.name = line.substr(std::min(line.size(), static_cast<size_t>(2 + used)));
    while (!p.name.empty() && (p.name.back() == '\r' || p.name.back() == ' ')) p.name.pop_back();
    break;
  }
  return true;
}

// ---------------------------------------------------------------- J-184 pictures

void pictureSize(int w, int h, int maxRows, int& outW, int& outH) {
  outW = outH = 0;
  if (w < 1 || h < 1) return;
  maxRows = std::clamp(maxRows, 1, kMaxRows);
  double nh = std::min(h, maxRows);
  double nw = static_cast<double>(w) * nh / static_cast<double>(h);
  if (nw > kMaxColumns) {
    nh = nh * kMaxColumns / nw;
    nw = kMaxColumns;
  }
  if (nw * nh > kMaxPixels) {
    const double k = std::sqrt(kMaxPixels / (nw * nh));
    nw *= k, nh *= k;
  }
  outW = std::clamp(static_cast<int>(std::lround(nw)), 1, std::min(w, kMaxColumns));
  outH = std::clamp(static_cast<int>(std::lround(nh)), 1, std::min(h, maxRows));
  while (static_cast<int64_t>(outW) * outH > kMaxPixels) outW > outH ? --outW : --outH;  // rounding up past the budget
}

bool pictureFromRgba(const uint8_t* rgba, int w, int h, int maxRows, Picture& out) {
  out = Picture{};
  if (!rgba || w < 1 || h < 1) return false;
  int W = 0, H = 0;
  pictureSize(w, h, maxRows, W, H);
  if (W < 1 || H < 1) return false;
  out.w = W, out.h = H, out.srcW = w, out.srcH = h;
  out.rgb.assign(static_cast<size_t>(W) * static_cast<size_t>(H) * 3, 0);
  // Box filter: each new row sums its band of source rows (colour × alpha, so
  // transparency lies on black), then each new pixel its band of columns.
  std::vector<uint32_t> acc(static_cast<size_t>(w) * 3);
  std::vector<int> xs(static_cast<size_t>(W) + 1);
  for (int x = 0; x <= W; ++x) xs[static_cast<size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * w / W);
  for (int Y = 0; Y < H; ++Y) {
    const int y0 = static_cast<int>(static_cast<int64_t>(Y) * h / H);
    const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(Y + 1) * h / H));
    std::fill(acc.begin(), acc.end(), 0u);
    for (int y = y0; y < y1; ++y) {
      const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
      uint32_t* a = acc.data();
      for (int x = 0; x < w; ++x, row += 4, a += 3) {
        const uint32_t al = row[3];
        a[0] += row[0] * al, a[1] += row[1] * al, a[2] += row[2] * al;
      }
    }
    for (int X = 0; X < W; ++X) {
      const int x0 = xs[static_cast<size_t>(X)], x1 = std::max(x0 + 1, xs[static_cast<size_t>(X) + 1]);
      uint64_t s[3] = {0, 0, 0};
      for (int x = x0; x < x1; ++x)
        for (int c = 0; c < 3; ++c) s[c] += acc[static_cast<size_t>(x) * 3 + static_cast<size_t>(c)];
      const uint64_t n = static_cast<uint64_t>(y1 - y0) * static_cast<uint64_t>(x1 - x0) * 255u;
      uint8_t* o = &out.rgb[(static_cast<size_t>(Y) * static_cast<size_t>(W) + static_cast<size_t>(X)) * 3];
      for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(std::min<uint64_t>(255, (s[c] + n / 2) / n));
    }
  }
  return true;
}

bool decodePicture(const std::string& bytes, int maxRows, Picture& out, std::string* err) {
  out = Picture{};
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (bytes.empty()) return fail("the file is empty");
  if (bytes.size() > kMaxFileBytes || bytes.size() > static_cast<size_t>(INT32_MAX)) return fail("the file is too large");
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const int len = static_cast<int>(bytes.size());
  int w = 0, h = 0;
  if (!gl_picture_info(data, len, &w, &h) || w < 1 || h < 1) return fail("not a picture Gridline can read (PNG, JPEG, BMP, GIF, TGA, PSD or HDR)");
  if (static_cast<int64_t>(w) * h > kMaxSourcePixels) return fail("the picture is too large (" + std::to_string(w) + " x " + std::to_string(h) + ")");
  unsigned char* px = gl_picture_decode(data, len, &w, &h);
  if (!px) {
    const char* why = gl_picture_failure();
    return fail(std::string("the picture could not be read") + (why && *why ? std::string(" (") + why + ")" : std::string()));
  }
  const bool ok = pictureFromRgba(px, w, h, maxRows, out);
  gl_picture_free(px);
  return ok || fail("the picture could not be read");
}

bool loadPictureFile(const std::string& path, int maxRows, Picture& out, std::string* err) {
  out = Picture{};
  namespace fs = std::filesystem;
  const fs::path fp = fs::u8path(path);
  std::error_code ec;
  if (!fs::is_regular_file(fp, ec)) {
    if (err) *err = "the file was not found";
    return false;
  }
  const auto size = fs::file_size(fp, ec);
  if (ec || size > kMaxFileBytes) {
    if (err) *err = ec ? "the file could not be read" : "the file is too large";
    return false;
  }
  std::ifstream in(fp, std::ios::binary);
  std::string bytes(static_cast<size_t>(size), '\0');
  if (!in || !in.read(&bytes[0], static_cast<std::streamsize>(bytes.size()))) {
    if (err) *err = "the file could not be read";
    return false;
  }
  if (!decodePicture(bytes, maxRows, out, err)) return false;
  out.name = fp.filename().u8string();
  return true;
}

const std::vector<std::string>& pictureExtensions() {
  static const std::vector<std::string> e = {".png", ".jpg", ".jpeg", ".jpe", ".bmp", ".gif", ".tga", ".psd", ".hdr"};
  return e;
}

bool isPictureFile(const std::string& path) {
  std::string e = std::filesystem::u8path(path).extension().u8string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto& v = pictureExtensions();
  return std::find(v.begin(), v.end(), e) != v.end();
}

void flipColumns(Picture& p) {
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w / 2; ++x)
      for (int c = 0; c < 3; ++c)
        std::swap(p.rgb[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)],
                  p.rgb[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(p.w - 1 - x)) * 3 + static_cast<size_t>(c)]);
}

void flipRows(Picture& p) {
  const size_t stride = static_cast<size_t>(p.w) * 3;
  for (int y = 0; y < p.h / 2; ++y)
    std::swap_ranges(p.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * stride),
                     p.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y + 1) * stride),
                     p.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(p.h - 1 - y) * stride));
}

void invertColours(Picture& p) {
  for (uint8_t& v : p.rgb) v = static_cast<uint8_t>(255 - v);
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

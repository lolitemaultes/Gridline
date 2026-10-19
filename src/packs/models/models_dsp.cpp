// Gridline — "models" pack: runtime registry and models shared by the
// runtimes and the editors (partial gains, image sources, body and mode tables).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include "../Packs.h"
#include "models.h"
#include "models_rt.h"

namespace gl::packs::models {

namespace {

float prm(const PluginState& s, int i, float d) {
  return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d;
}

// Deterministic hash → 0..1.
float hash01(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

}  // namespace

// ============================================================ images
namespace {

std::mutex& imgMutex() {
  static std::mutex m;
  return m;
}

std::shared_ptr<Image> blank(int w, int h) {
  auto im = std::make_shared<Image>();
  im->w = w;
  im->h = h;
  im->r.assign(static_cast<size_t>(w * h), 0.0f);
  im->g = im->r;
  im->b = im->r;
  return im;
}

void plot(Image& im, int x, int y, float r, float g, float b) {
  if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
  size_t i = static_cast<size_t>(y * im.w + x);
  im.r[i] = std::min(1.0f, im.r[i] + r);
  im.g[i] = std::min(1.0f, im.g[i] + g);
  im.b[i] = std::min(1.0f, im.b[i] + b);
}

ImagePtr makePattern(int p) {
  const int W = 96, H = 48;
  auto im = blank(W, H);
  switch (p) {
    case 0:  // rising sweep with a fainter harmonic above
      for (int x = 0; x < W; ++x) {
        int y = H - 1 - x * (H - 8) / W;
        plot(*im, x, y, 1, 0.8f, 0);
        plot(*im, x, y + 1, 0.4f, 0.3f, 0);
        plot(*im, x, y - 6, 0.25f, 0.35f, 0.2f);
      }
      break;
    case 1: {  // chord ladder: four chords of stacked bars
      static const int rows[4][4] = {{40, 33, 28, 21}, {38, 31, 26, 19}, {41, 34, 29, 24}, {36, 29, 24, 17}};
      for (int c = 0; c < 4; ++c)
        for (int x = c * 24; x < c * 24 + 22; ++x) {
          float fade = 1.0f - 0.6f * static_cast<float>(x - c * 24) / 22.0f;
          for (int v = 0; v < 4; ++v) plot(*im, x, rows[c][v], fade * (v & 1 ? 0.8f : 1.0f), fade * (v & 1 ? 1.0f : 0.7f), 0);
        }
      break;
    }
    case 2:  // rain: scattered short drops
      for (int d = 0; d < 140; ++d) {
        int x = static_cast<int>(hash01(11, static_cast<uint32_t>(d)) * W);
        int y = static_cast<int>(hash01(23, static_cast<uint32_t>(d)) * H);
        float a = 0.4f + 0.6f * hash01(37, static_cast<uint32_t>(d));
        bool left = hash01(51, static_cast<uint32_t>(d)) < 0.5f;
        for (int k = 0; k < 3; ++k) plot(*im, x + k, y + k / 2, left ? a : a * 0.3f, left ? a * 0.3f : a, 0);
      }
      break;
    case 3:  // rings
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          float dx = (x - W * 0.5f) / (W * 0.5f), dy = (y - H * 0.5f) / (H * 0.5f);
          float d = std::sqrt(dx * dx + dy * dy) * 4.0f;
          float v = std::pow(0.5f + 0.5f * std::cos(d * 6.2831853f), 12.0f) * std::max(0.0f, 1.0f - d / 4.5f);
          plot(*im, x, y, v, v * 0.6f, v * 0.3f);
        }
      break;
    case 4:  // stairs
      for (int s = 0; s < 8; ++s)
        for (int x = s * 12; x < s * 12 + 11; ++x)
          for (int t = 0; t < 2; ++t) plot(*im, x, H - 6 - s * 5 - t, 0.9f, s & 1 ? 0.5f : 0.9f, 0);
      break;
    case 5:  // waves
      for (int x = 0; x < W; ++x)
        for (int k = 0; k < 3; ++k) {
          float y = H * (0.3f + 0.2f * k) + 5.0f * std::sin(x * 0.18f + k * 1.7f);
          plot(*im, x, static_cast<int>(y), k == 1 ? 0.5f : 1.0f, k == 1 ? 1.0f : 0.5f, 0);
        }
      break;
    case 6:  // pulse grid: rhythmic blocks on a few rows
      for (int x = 0; x < W; ++x) {
        if (x % 8 < 2) plot(*im, x, 42, 1, 1, 0);
        if (x % 8 >= 4 && x % 8 < 5) plot(*im, x, 30, 0.8f, 0.4f, 0);
        if (x % 4 == 2) plot(*im, x, 8, 0.25f, 0.5f, 0);
        if (x % 16 == 12) plot(*im, x, 22, 0.6f, 0.6f, 0.3f);
      }
      break;
    default:  // drifting cloud
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          float n = hash01(static_cast<uint32_t>(x / 3), static_cast<uint32_t>(y / 2 + 100));
          float env = std::exp(-std::pow((y - H * (0.75f - 0.4f * x / W)) / 7.0f, 2.0f));
          float v = env * n * 0.8f;
          plot(*im, x, y, v, v * hash01(static_cast<uint32_t>(x), 7), 0);
        }
      break;
  }
  return im;
}

ImagePtr downscale(int sw, int sh, const std::vector<float>& r, const std::vector<float>& g, const std::vector<float>& b) {
  int w = std::min(sw, kImgMaxCols), h = std::min(sh, kImgMaxRows);
  auto im = blank(w, h);
  for (int y = 0; y < h; ++y) {
    int y0 = y * sh / h, y1 = std::max(y0 + 1, (y + 1) * sh / h);
    for (int x = 0; x < w; ++x) {
      int x0 = x * sw / w, x1 = std::max(x0 + 1, (x + 1) * sw / w);
      float ar = 0, ag = 0, ab = 0;
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
          size_t i = static_cast<size_t>(yy * sw + xx);
          ar += r[i];
          ag += g[i];
          ab += b[i];
        }
      float n = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
      size_t o = static_cast<size_t>(y * w + x);
      im->r[o] = ar * n;
      im->g[o] = ag * n;
      im->b[o] = ab * n;
    }
  }
  return im;
}

uint32_t rd16(const unsigned char* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) { return rd16(p) | (rd16(p + 2) << 16); }

ImagePtr decodeBmp(const std::vector<unsigned char>& f, std::string* err) {
  if (f.size() < 54) return nullptr;
  uint32_t off = rd32(&f[10]), hdr = rd32(&f[14]);
  int w = static_cast<int32_t>(rd32(&f[18]));
  int hs = static_cast<int32_t>(rd32(&f[22]));
  int bpp = static_cast<int>(rd16(&f[28]));
  uint32_t comp = rd32(&f[30]);
  bool topDown = hs < 0;
  int h = std::abs(hs);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (comp != 0 && !(comp == 3 && bpp == 32)) || (bpp != 8 && bpp != 24 && bpp != 32)) {
    if (err) *err = "unsupported BMP (needs uncompressed 8/24/32-bit)";
    return nullptr;
  }
  size_t stride = (static_cast<size_t>(w) * static_cast<size_t>(bpp) / 8 + 3) & ~static_cast<size_t>(3);
  if (off + stride * static_cast<size_t>(h) > f.size()) return nullptr;
  const unsigned char* pal = &f[14 + hdr];
  size_t n = static_cast<size_t>(w * h);
  std::vector<float> r(n), g(n), b(n);
  for (int y = 0; y < h; ++y) {
    const unsigned char* row = &f[off + stride * static_cast<size_t>(topDown ? y : h - 1 - y)];
    for (int x = 0; x < w; ++x) {
      unsigned char cr, cg, cb;
      if (bpp == 8) {
        const unsigned char* c = pal + 4 * row[x];
        if (c + 3 > f.data() + f.size()) return nullptr;
        cb = c[0], cg = c[1], cr = c[2];
      } else {
        const unsigned char* c = row + x * (bpp / 8);
        cb = c[0], cg = c[1], cr = c[2];
      }
      size_t i = static_cast<size_t>(y * w + x);
      r[i] = cr / 255.0f, g[i] = cg / 255.0f, b[i] = cb / 255.0f;
    }
  }
  return downscale(w, h, r, g, b);
}

ImagePtr decodePnm(const std::vector<unsigned char>& f) {
  size_t p = 2;
  auto token = [&]() -> long {
    while (p < f.size()) {
      if (f[p] == '#') {
        while (p < f.size() && f[p] != '\n') ++p;
      } else if (std::isspace(f[p])) {
        ++p;
      } else {
        break;
      }
    }
    long v = 0;
    bool any = false;
    while (p < f.size() && f[p] >= '0' && f[p] <= '9') v = v * 10 + (f[p++] - '0'), any = true;
    return any ? v : -1;
  };
  bool color = f[1] == '6';
  long w = token(), h = token(), mx = token();
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || mx <= 0 || mx > 255) return nullptr;
  ++p;  // single whitespace before raster
  size_t ch = color ? 3 : 1, n = static_cast<size_t>(w * h);
  if (p + n * ch > f.size()) return nullptr;
  std::vector<float> r(n), g(n), b(n);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char* c = &f[p + i * ch];
    r[i] = c[0] / static_cast<float>(mx);
    g[i] = c[color ? 1 : 0] / static_cast<float>(mx);
    b[i] = c[color ? 2 : 0] / static_cast<float>(mx);
  }
  return downscale(static_cast<int>(w), static_cast<int>(h), r, g, b);
}

ImagePtr decodeTga(const std::vector<unsigned char>& f) {
  if (f.size() < 18) return nullptr;
  int idLen = f[0], type = f[2], w = static_cast<int>(rd16(&f[12])), h = static_cast<int>(rd16(&f[14])), bpp = f[16];
  bool topDown = (f[17] & 0x20) != 0;
  if ((type != 2 && type != 3) || w <= 0 || h <= 0 || (bpp != 8 && bpp != 24 && bpp != 32) || f[1] != 0) return nullptr;
  size_t px = static_cast<size_t>(bpp / 8), off = 18 + static_cast<size_t>(idLen), n = static_cast<size_t>(w * h);
  if (off + n * px > f.size()) return nullptr;
  std::vector<float> r(n), g(n), b(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const unsigned char* c = &f[off + (static_cast<size_t>(topDown ? y : h - 1 - y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * px];
      size_t i = static_cast<size_t>(y * w + x);
      if (px == 1)
        r[i] = g[i] = b[i] = c[0] / 255.0f;
      else
        b[i] = c[0] / 255.0f, g[i] = c[1] / 255.0f, r[i] = c[2] / 255.0f;
    }
  return downscale(w, h, r, g, b);
}

}  // namespace

ImagePtr builtinImage(int pattern) {
  static const std::vector<ImagePtr> all = [] {
    std::vector<ImagePtr> v;
    for (int i = 0; i < kImgPatterns; ++i) v.push_back(makePattern(i));
    return v;
  }();
  return all[static_cast<size_t>(std::clamp(pattern, 0, kImgPatterns - 1))];
}

ImagePtr loadImageFile(const std::string& path, std::string* err) {
  std::lock_guard<std::mutex> lk(imgMutex());
  static std::map<std::string, ImagePtr> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open file";
    return nullptr;
  }
  std::vector<unsigned char> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ImagePtr im;
  if (f.size() > 2 && f[0] == 'B' && f[1] == 'M')
    im = decodeBmp(f, err);
  else if (f.size() > 2 && f[0] == 'P' && (f[1] == '5' || f[1] == '6'))
    im = decodePnm(f);
  else
    im = decodeTga(f);
  if (!im && err && err->empty()) *err = "unsupported image (use BMP, PPM/PGM or TGA)";
  if (im) cache[path] = im;
  return im;
}

std::vector<uint8_t> decodeCanvas(const std::string& hex) {
  std::vector<uint8_t> px(static_cast<size_t>(kCanvasW * kCanvasH), 0);
  for (size_t i = 0; i < px.size() && i < hex.size(); ++i) {
    char c = hex[i];
    px[i] = static_cast<uint8_t>(c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0));
  }
  return px;
}

std::string encodeCanvas(const std::vector<uint8_t>& px) {
  static const char* digits = "0123456789abcdef";
  std::string s(static_cast<size_t>(kCanvasW * kCanvasH), '0');
  for (size_t i = 0; i < s.size() && i < px.size(); ++i) s[i] = digits[px[i] & 15];
  // Trailing zeros are implied.
  size_t e = s.find_last_not_of('0');
  s.resize(e == std::string::npos ? 1 : e + 1);  // a blank canvas is "0" ("" = starter sketch)
  return s;
}

std::vector<uint8_t> starterCanvas() {
  // A small original sketch: a falling line, a held low tone and dotted high notes.
  std::vector<uint8_t> px(static_cast<size_t>(kCanvasW * kCanvasH), 0);
  for (int x = 0; x < kCanvasW; ++x) {
    int y = 4 + x * 18 / kCanvasW;
    px[static_cast<size_t>(y * kCanvasW + x)] = 15;
    if (y + 1 < kCanvasH) px[static_cast<size_t>((y + 1) * kCanvasW + x)] = 6;
    px[static_cast<size_t>(27 * kCanvasW + x)] = static_cast<uint8_t>(x % 16 < 12 ? 9 : 0);
    if (x % 8 == 0) px[static_cast<size_t>(2 * kCanvasW + x)] = 12;
  }
  return px;
}

ImagePtr canvasImage(const std::string& hex) {
  std::lock_guard<std::mutex> lk(imgMutex());
  static std::map<std::string, ImagePtr> cache;
  auto it = cache.find(hex);
  if (it != cache.end()) return it->second;
  if (cache.size() > 64) cache.clear();
  auto px = hex.empty() ? starterCanvas() : decodeCanvas(hex);
  auto im = blank(kCanvasW, kCanvasH);
  for (size_t i = 0; i < px.size(); ++i) im->r[i] = im->g[i] = px[i] / 15.0f;
  cache[hex] = im;
  return im;
}

std::string imageDataPath(const std::string& data) { return data.substr(0, data.find('\n')); }
std::string imageDataCanvas(const std::string& data) {
  size_t p = data.find('\n');
  return p == std::string::npos ? std::string() : data.substr(p + 1);
}
std::string makeImageData(const std::string& path, const std::string& canvas) { return canvas.empty() ? path : path + "\n" + canvas; }

ImagePtr imageFor(const PluginState& s) {
  int src = static_cast<int>(prm(s, ImSource, 0) + 0.5f);
  if (src == SrcFile) {
    std::string p = imageDataPath(s.data);
    if (!p.empty())
      if (auto im = loadImageFile(p)) return im;
    return builtinImage(static_cast<int>(prm(s, ImPattern, 0) + 0.5f));
  }
  if (src == SrcCanvas) return canvasImage(imageDataCanvas(s.data));
  return builtinImage(static_cast<int>(prm(s, ImPattern, 0) + 0.5f));
}

double imageRowHz(int row, int rows, int scale, double low, double high, double f0Ratio) {
  double y = rows > 1 ? 1.0 - static_cast<double>(row) / (rows - 1) : 0.0;
  double hz;
  if (scale == 1)
    hz = low + (high - low) * y;
  else if (scale == 2)
    hz = low * static_cast<double>(rows - row);
  else
    hz = low * std::pow(std::max(1.0001, high / low), y);
  return hz * f0Ratio;
}

// ============================================================ string / drum tables
const BodyMode* bodyModes(int body) {
  static const BodyMode t[kBodies][kBodyModes] = {
      {{100, 0, 1}, {200, 0, 1}, {400, 0, 1}, {800, 0, 1}, {1600, 0, 1}, {3200, 0, 1}},
      {{102, 1.0f, 9}, {205, 0.9f, 11}, {390, 0.55f, 8}, {560, 0.4f, 7}, {940, 0.3f, 5}, {2600, 0.25f, 3}},       // acoustic
      {{96, 0.9f, 10}, {188, 1.0f, 12}, {360, 0.5f, 9}, {470, 0.45f, 8}, {820, 0.25f, 5}, {2100, 0.15f, 3}},       // nylon
      {{285, 0.8f, 12}, {455, 1.0f, 14}, {560, 0.8f, 12}, {1050, 0.4f, 6}, {2450, 0.6f, 3}, {3300, 0.35f, 4}},     // violin
      {{108, 0.9f, 10}, {215, 1.0f, 12}, {290, 0.7f, 10}, {610, 0.45f, 7}, {1020, 0.3f, 5}, {2050, 0.25f, 3}},     // cello
      {{150, 0.7f, 14}, {330, 0.9f, 16}, {660, 0.8f, 12}, {1210, 0.5f, 9}, {2230, 0.35f, 6}, {3650, 0.2f, 5}},     // long zither
      {{122, 1.0f, 7}, {250, 0.8f, 8}, {385, 0.6f, 9}, {810, 0.35f, 6}, {1520, 0.25f, 5}, {3050, 0.2f, 4}},        // wooden box
      {{610, 0.8f, 25}, {1220, 0.9f, 28}, {1830, 0.6f, 25}, {2710, 0.5f, 22}, {3740, 0.4f, 20}, {5210, 0.3f, 18}}  // metal cone
  };
  return t[std::clamp(body, 0, kBodies - 1)];
}

int strumChord(int chord, int* semis) {
  static const int chords[7][kStrumMax] = {{0, 0, 0, 0, 0, 0},      {0, 7, 12, 16, 19, 24}, {0, 7, 12, 15, 19, 24}, {0, 7, 12, 19, 0, 0},
                                           {0, 7, 10, 16, 19, 24}, {0, 7, 12, 17, 19, 24}, {0, 12, 24, 0, 0, 0}};
  static const int counts[7] = {1, 6, 6, 4, 6, 6, 3};
  chord = std::clamp(chord, 0, 6);
  for (int i = 0; i < counts[chord]; ++i) semis[i] = chords[chord][i];
  return counts[chord];
}

double stringRatio(int choice) {
  static const double r[6] = {1.0, 1.0, 2.0, 1.5, 0.5, 3.0};
  return r[std::clamp(choice, 0, 5)];
}

const float* drumRatios(int shape) {
  static const float r[DsCount][kDrumModes] = {
      {1.0f, 1.593f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f, 3.600f, 3.652f, 4.060f, 4.154f},  // membrane
      {1.0f, 1.47f, 1.98f, 2.44f, 2.93f, 3.42f, 3.91f, 4.36f, 4.85f, 5.30f, 5.80f, 6.30f},              // tom (air loaded)
      {1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.81f, 31.87f, 39.81f, 48.6f, 58.3f, 68.9f},      // free bar
      {1.0f, 1.62f, 2.06f, 2.65f, 2.97f, 3.55f, 4.01f, 4.62f, 5.17f, 5.63f, 6.31f, 6.95f},              // plate
      {0.5f, 1.0f, 1.19f, 1.5f, 2.0f, 2.51f, 2.66f, 3.01f, 4.17f, 5.43f, 6.0f, 6.8f},                   // bell
      {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f}};                     // tabla (harmonic)
  return r[std::clamp(shape, 0, DsCount - 1)];
}

const float* drumGains(int shape) {
  static const float g[DsCount][kDrumModes] = {
      {1.0f, 0.75f, 0.6f, 0.5f, 0.42f, 0.36f, 0.3f, 0.26f, 0.22f, 0.2f, 0.17f, 0.15f},
      {1.0f, 0.6f, 0.45f, 0.35f, 0.28f, 0.22f, 0.18f, 0.15f, 0.12f, 0.1f, 0.08f, 0.07f},
      {1.0f, 0.55f, 0.35f, 0.25f, 0.18f, 0.12f, 0.08f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f},
      {1.0f, 0.9f, 0.85f, 0.8f, 0.75f, 0.7f, 0.66f, 0.62f, 0.58f, 0.55f, 0.52f, 0.5f},
      {0.7f, 1.0f, 0.6f, 0.75f, 0.5f, 0.45f, 0.3f, 0.35f, 0.25f, 0.2f, 0.15f, 0.12f},
      {1.0f, 0.65f, 0.5f, 0.35f, 0.25f, 0.18f, 0.12f, 0.08f, 0.06f, 0.04f, 0.03f, 0.02f}};
  return g[std::clamp(shape, 0, DsCount - 1)];
}

const char* drumPadName(int pad) {
  static const char* n[kDrumPads] = {"Kick", "Snare", "Rim", "Clap", "Closed hat", "Open hat", "Low tom", "Mid tom",
                                     "High tom", "Crash", "Cowbell", "Clave", "Conga", "Bongo", "Tabla", "Timpani"};
  return n[std::clamp(pad, 0, kDrumPads - 1)];
}

// J-22 groups (drums_dsp.cpp, strings_dsp.cpp, metal_dsp.cpp).
std::unique_ptr<Generator> createDrumsGenerator(const std::string& type);
bool prepareDrumsAux(const PluginState& state, ChannelAux& aux);
std::unique_ptr<Generator> createStringsGenerator(const std::string& type);
bool prepareStringsAux(const PluginState& state, ChannelAux& aux);
std::unique_ptr<Generator> createMetalGenerator(const std::string& type);
bool prepareMetalAux(const PluginState& state, ChannelAux& aux);

// ============================================================ registry
std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == "gl.additive") return makeAdditive();
  if (type == "gl.harmor") return makeHarmor();
  if (type == "gl.imagesynth") return makeImageSynth();
  if (type == "gl.string") return makeString();
  if (type == "gl.drumkit") return makeDrumKit();
  if (auto g = createDrumsGenerator(type)) return g;
  if (auto g = createStringsGenerator(type)) return g;
  if (auto g = createMetalGenerator(type)) return g;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  (void)type;
  return nullptr;
}

// Image Synth: the file and canvas pictures travel to the audio thread as
// samples (left = red, right = green; second sample left = blue; the image
// width is stored in sampleRate). Order: [0,1] file, [2,3] canvas.
bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (prepareDrumsAux(state, aux) || prepareStringsAux(state, aux) || prepareMetalAux(state, aux)) return true;
  if (state.type == "gl.additive") {
    aux = ChannelAux{};
    aux.custom = buildAdditiveAux(state);
    return true;
  }
  if (state.type == "gl.harmor") {
    aux = ChannelAux{};
    aux.custom = buildHarmorAux(state);
    return true;
  }
  if (state.type == "gl.string" || state.type == "gl.drumkit") {
    aux = ChannelAux{};
    return true;
  }
  if (state.type != "gl.imagesynth") return false;
  aux = ChannelAux{};
  auto pack = [](const ImagePtr& im, SamplePtr& rg, SamplePtr& bl) {
    if (!im) return;
    std::lock_guard<std::mutex> lk(imgMutex());
    static std::map<const Image*, std::pair<SamplePtr, SamplePtr>> conv;
    static std::vector<ImagePtr> keep;  // keeps converted images alive so keys stay unique
    auto it = conv.find(im.get());
    if (it == conv.end()) {
      if (keep.size() > 256) {  // drop old conversions (running programs keep their own references)
        conv.clear();
        keep.clear();
      }
      auto a = std::make_shared<Sample>();
      a->path = "image";
      a->sampleRate = im->w;
      a->left = im->r;
      a->right = im->g;
      auto b = std::make_shared<Sample>();
      b->path = "image-blue";
      b->sampleRate = im->w;
      b->left = im->b;
      b->right = im->b;
      it = conv.emplace(im.get(), std::make_pair(SamplePtr(a), SamplePtr(b))).first;
      keep.push_back(im);
    }
    rg = it->second.first;
    bl = it->second.second;
  };
  aux.samples.resize(4);
  std::string path = imageDataPath(state.data);
  if (!path.empty()) pack(loadImageFile(path), aux.samples[0], aux.samples[1]);
  pack(canvasImage(imageDataCanvas(state.data)), aux.samples[2], aux.samples[3]);
  return true;
}

}  // namespace gl::packs::models

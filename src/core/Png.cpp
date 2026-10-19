// Gridline — minimal PNG codec (see Png.h).
#include "Png.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include "flp/FlpFx.h"

namespace gl::png {

namespace {

struct CrcTable {
  uint32_t t[256];
  CrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
  }
};

uint32_t crcTable(size_t i) {
  static const CrcTable table;  // thread-safe one-time init
  return table.t[i];
}

uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) c = crcTable((c ^ p[i]) & 0xFF) ^ (c >> 8);
  return c;
}

void put32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 24));
  o.push_back(static_cast<uint8_t>(v >> 16));
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}

uint32_t get32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
  put32(o, static_cast<uint32_t>(data.size()));
  size_t start = o.size();
  o.insert(o.end(), type, type + 4);
  o.insert(o.end(), data.begin(), data.end());
  put32(o, crc(o.data() + start, o.size() - start) ^ 0xFFFFFFFFu);
}

struct BitWriter {
  std::vector<uint8_t>& out;
  uint32_t acc = 0;
  int n = 0;
  void bits(uint32_t v, int count) {  // LSB first
    acc |= v << n;
    n += count;
    while (n >= 8) {
      out.push_back(static_cast<uint8_t>(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void huff(uint32_t code, int len) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < len; ++i) r |= ((code >> i) & 1u) << (len - 1 - i);
    bits(r, len);
  }
  void flush() {
    if (n > 0) out.push_back(static_cast<uint8_t>(acc));
    acc = 0;
    n = 0;
  }
};

void literal(BitWriter& w, int v) {
  if (v < 144) w.huff(static_cast<uint32_t>(0x30 + v), 8);
  else if (v < 256) w.huff(static_cast<uint32_t>(0x190 + v - 144), 9);
  else if (v < 280) w.huff(static_cast<uint32_t>(v - 256), 7);
  else w.huff(static_cast<uint32_t>(0xC0 + v - 280), 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void match(BitWriter& w, int len, int dist) {
  int li = 28;
  while (li > 0 && kLenBase[li] > len) --li;
  literal(w, 257 + li);
  if (kLenExtra[li]) w.bits(static_cast<uint32_t>(len - kLenBase[li]), kLenExtra[li]);
  int di = 29;
  while (di > 0 && kDistBase[di] > dist) --di;
  w.huff(static_cast<uint32_t>(di), 5);
  if (kDistExtra[di]) w.bits(static_cast<uint32_t>(dist - kDistBase[di]), kDistExtra[di]);
}

int paeth(int a, int b, int c) {
  int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

std::vector<uint8_t> deflateZlib(const uint8_t* data, size_t n) {
  std::vector<uint8_t> out = {0x78, 0x01};
  BitWriter w{out};
  w.bits(1, 1);  // final block
  w.bits(1, 2);  // fixed Huffman
  constexpr int kHashBits = 15, kWindow = 32768, kMaxChain = 48;
  std::vector<int> head(1u << kHashBits, -1), prev(n, -1);
  auto hash = [&](size_t i) { return ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & ((1 << kHashBits) - 1); };
  size_t i = 0;
  while (i < n) {
    int bestLen = 0, bestDist = 0;
    if (i + 2 < n) {
      int h = hash(i);
      int cand = head[static_cast<size_t>(h)];
      for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain, cand = prev[static_cast<size_t>(cand)]) {
        int dist = static_cast<int>(i) - cand;
        if (dist > kWindow) break;
        int len = 0;
        const size_t maxLen = std::min<size_t>(258, n - i);
        while (static_cast<size_t>(len) < maxLen && data[static_cast<size_t>(cand) + static_cast<size_t>(len)] == data[i + static_cast<size_t>(len)]) ++len;
        if (len > bestLen) bestLen = len, bestDist = dist;
        if (len == 258) break;
      }
    }
    size_t adv = bestLen >= 3 ? static_cast<size_t>(bestLen) : 1;
    if (bestLen >= 3) match(w, bestLen, bestDist);
    else literal(w, data[i]);
    for (size_t k = 0; k < adv; ++k, ++i)
      if (i + 2 < n) {
        int h = hash(i);
        prev[i] = head[static_cast<size_t>(h)];
        head[static_cast<size_t>(h)] = static_cast<int>(i);
      }
  }
  literal(w, 256);
  w.flush();
  uint32_t a = 1, b = 0;
  for (size_t k = 0; k < n; ++k) {
    a = (a + data[k]) % 65521;
    b = (b + a) % 65521;
  }
  put32(out, (b << 16) | a);
  return out;
}

std::vector<uint8_t> encode(const Image& img) {
  std::vector<uint8_t> out;
  if (img.width <= 0 || img.height <= 0 || (img.channels != 3 && img.channels != 4) ||
      img.pixels.size() < static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * static_cast<size_t>(img.channels))
    return out;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.assign(sig, sig + 8);
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(img.width));
  put32(ihdr, static_cast<uint32_t>(img.height));
  ihdr.push_back(8);
  ihdr.push_back(img.channels == 4 ? 6 : 2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(out, "IHDR", ihdr);
  const size_t stride = static_cast<size_t>(img.width) * static_cast<size_t>(img.channels);
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<size_t>(img.height));
  for (int y = 0; y < img.height; ++y) {
    const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * stride;
    raw.push_back(1);  // Sub
    for (size_t x = 0; x < stride; ++x)
      raw.push_back(static_cast<uint8_t>(row[x] - (x >= static_cast<size_t>(img.channels) ? row[x - static_cast<size_t>(img.channels)] : 0)));
  }
  chunk(out, "IDAT", deflateZlib(raw.data(), raw.size()));
  chunk(out, "IEND", {});
  return out;
}

bool decode(const uint8_t* data, size_t n, Image& out) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (n < 8 || std::memcmp(data, sig, 8) != 0) return false;
  size_t p = 8;
  int w = 0, h = 0, ch = 0;
  std::vector<uint8_t> idat;
  while (p + 12 <= n) {
    uint32_t len = get32(data + p);
    if (len > n - p - 12) return false;
    const uint8_t* type = data + p + 4;
    const uint8_t* body = data + p + 8;
    if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
      w = static_cast<int>(get32(body));
      h = static_cast<int>(get32(body + 4));
      if (body[8] != 8 || body[12] != 0 || body[10] != 0) return false;
      ch = body[9] == 2 ? 3 : body[9] == 6 ? 4 : 0;
      if (!ch || w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), body, body + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    p += 12 + len;
  }
  if (!ch || idat.empty()) return false;
  const size_t stride = static_cast<size_t>(w) * static_cast<size_t>(ch);
  const size_t need = (stride + 1) * static_cast<size_t>(h);
  std::vector<uint8_t> raw = flp::fx::inflateZlib(idat.data(), idat.size(), need + 16);
  if (raw.size() < need) return false;
  out.width = w;
  out.height = h;
  out.channels = ch;
  out.pixels.assign(stride * static_cast<size_t>(h), 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t f = raw[static_cast<size_t>(y) * (stride + 1)];
    const uint8_t* src = raw.data() + static_cast<size_t>(y) * (stride + 1) + 1;
    uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * stride;
    const uint8_t* up = y > 0 ? dst - stride : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      int a = x >= static_cast<size_t>(ch) ? dst[x - static_cast<size_t>(ch)] : 0;
      int b = up ? up[x] : 0;
      int c = up && x >= static_cast<size_t>(ch) ? up[x - static_cast<size_t>(ch)] : 0;
      int v = src[x];
      switch (f) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: v += paeth(a, b, c); break;
        default: return false;
      }
      dst[x] = static_cast<uint8_t>(v);
    }
  }
  return true;
}

bool writeFile(const std::string& path, const Image& img) {
  std::vector<uint8_t> bytes = encode(img);
  if (bytes.empty()) return false;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

bool readFile(const std::string& path, Image& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return decode(bytes.data(), bytes.size(), out);
}

Image resize(const Image& src, int w, int h) {
  Image o;
  if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0) return o;
  o.width = w;
  o.height = h;
  o.channels = src.channels;
  const size_t C = static_cast<size_t>(src.channels);
  o.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * C, 0);
  for (int y = 0; y < h; ++y) {
    int y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
    for (int x = 0; x < w; ++x) {
      int x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
      for (size_t c = 0; c < C; ++c) {
        uint32_t sum = 0, cnt = 0;
        for (int yy = y0; yy < y1; ++yy)
          for (int xx = x0; xx < x1; ++xx) {
            sum += src.pixels[(static_cast<size_t>(yy) * static_cast<size_t>(src.width) + static_cast<size_t>(xx)) * C + c];
            ++cnt;
          }
        o.pixels[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * C + c] = static_cast<uint8_t>(sum / std::max(1u, cnt));
      }
    }
  }
  return o;
}

}  // namespace gl::png

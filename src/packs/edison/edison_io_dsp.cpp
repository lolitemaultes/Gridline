// Gridline — "edison" pack: files and state — WAV reader / writer with cue
// markers, regions and loop points, a FLAC encoder (fixed predictors + Rice
// codes), content hashing, the JSON plugin state and the FL Studio Edison
// state decoder (header fields + embedded RIFF/WAVE, layout read from FL's
// own shipped presets).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../../core/Json.h"
#include "edison.h"
#include "edison_internal.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ byte helpers
void put16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  put16(b, v & 0xFFFF);
  put16(b, v >> 16);
}
void putTag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
uint32_t rd16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }

bool writeBytes(const std::string& path, const std::vector<uint8_t>& b, std::string* err) {
  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  if (!f) {
    if (err) *err = "write failed: " + path;
    return false;
  }
  return true;
}

int32_t quantize(float x, int bits) {
  const double full = static_cast<double>(1 << (bits - 1));
  double v = std::nearbyint(static_cast<double>(x) * full);
  return static_cast<int32_t>(std::clamp(v, -full, full - 1));
}

std::string toHex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
  return s;
}

}  // namespace

// ------------------------------------------------------------------ WAV
bool writeWavFile(const std::string& path, const Content& c, int bits, std::string* err) {
  const Audio& a = c.a();
  if (bits != 16 && bits != 24 && bits != 32) bits = 32;
  const bool flt = bits == 32;
  const uint32_t chans = static_cast<uint32_t>(a.channels == 1 ? 1 : 2);
  const uint32_t bps = static_cast<uint32_t>(bits / 8), align = bps * chans;
  const uint64_t dataBytes = static_cast<uint64_t>(a.frames()) * align;
  if (dataBytes > 0xFFFFFFF0ull) {
    if (err) *err = "audio too long for a WAV file";
    return false;
  }
  std::vector<uint8_t> b;
  b.reserve(static_cast<size_t>(dataBytes) + 1024);
  putTag(b, "RIFF");
  put32(b, 0);  // patched below
  putTag(b, "WAVE");
  putTag(b, "fmt ");
  put32(b, 16);
  put16(b, flt ? 3 : 1);
  put16(b, chans);
  put32(b, static_cast<uint32_t>(std::lround(a.rate)));
  put32(b, static_cast<uint32_t>(std::lround(a.rate)) * align);
  put16(b, align);
  put16(b, static_cast<uint32_t>(bits));
  putTag(b, "data");
  put32(b, static_cast<uint32_t>(dataBytes));
  for (int64_t i = 0; i < a.frames(); ++i)
    for (uint32_t ch = 0; ch < chans; ++ch) {
      float x = a.ch(static_cast<int>(ch))[i];
      if (flt) {
        uint32_t u;
        std::memcpy(&u, &x, 4);
        put32(b, u);
      } else {
        int32_t q = quantize(x, bits);
        b.push_back(static_cast<uint8_t>(q));
        b.push_back(static_cast<uint8_t>(q >> 8));
        if (bits == 24) b.push_back(static_cast<uint8_t>(q >> 16));
      }
    }
  if (dataBytes & 1) b.push_back(0);
  // Cue points: markers, then regions (with a labelled-text length).
  const size_t nCue = c.markers.size() + c.regions.size();
  if (nCue > 0) {
    putTag(b, "cue ");
    put32(b, static_cast<uint32_t>(4 + 24 * nCue));
    put32(b, static_cast<uint32_t>(nCue));
    uint32_t id = 1;
    auto cue = [&](int64_t pos) {
      put32(b, id++);
      put32(b, static_cast<uint32_t>(pos));
      putTag(b, "data");
      put32(b, 0);
      put32(b, 0);
      put32(b, static_cast<uint32_t>(pos));
    };
    for (const auto& m : c.markers) cue(m.pos);
    for (const auto& g : c.regions) cue(g.start);
    std::vector<uint8_t> l;
    putTag(l, "adtl");
    auto labl = [&](uint32_t cid, const std::string& name) {
      putTag(l, "labl");
      uint32_t sz = static_cast<uint32_t>(4 + name.size() + 1);
      put32(l, sz);
      put32(l, cid);
      l.insert(l.end(), name.begin(), name.end());
      l.push_back(0);
      if (sz & 1) l.push_back(0);
    };
    id = 1;
    for (const auto& m : c.markers) labl(id++, m.name);
    for (const auto& g : c.regions) {
      putTag(l, "ltxt");
      put32(l, 20);
      put32(l, id);
      put32(l, static_cast<uint32_t>(g.end - g.start));
      putTag(l, "rgn ");
      put32(l, 0);
      put32(l, 0);
      labl(id++, g.name);
    }
    putTag(b, "LIST");
    put32(b, static_cast<uint32_t>(l.size()));
    b.insert(b.end(), l.begin(), l.end());
  }
  if (c.hasLoop()) {
    putTag(b, "smpl");
    put32(b, 36 + 24);
    put32(b, 0);
    put32(b, 0);
    put32(b, static_cast<uint32_t>(1e9 / std::max(1.0, a.rate)));
    put32(b, 60);
    for (int k = 0; k < 3; ++k) put32(b, 0);
    put32(b, 1);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put32(b, static_cast<uint32_t>(c.loopStart));
    put32(b, static_cast<uint32_t>(c.loopEnd - 1));
    put32(b, 0);
    put32(b, 0);
  }
  const uint32_t riff = static_cast<uint32_t>(b.size() - 8);
  std::memcpy(b.data() + 4, &riff, 4);  // little-endian hosts (macOS / Linux targets)
  return writeBytes(path, b, err);
}

bool parseWav(const uint8_t* p, size_t n, Content& out, std::string* err) {
  if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
    if (err) *err = "not a RIFF/WAVE file";
    return false;
  }
  uint32_t tag = 0, chans = 0, rate = 0, bits = 0;
  const uint8_t* data = nullptr;
  size_t dataLen = 0;
  struct Cue {
    uint32_t id, pos;
  };
  std::vector<Cue> cues;
  std::map<uint32_t, std::string> labels;
  std::map<uint32_t, uint32_t> lengths;
  int64_t loopS = -1, loopE = -1;
  size_t o = 12;
  while (o + 8 <= n) {
    const uint8_t* ck = p + o;
    uint32_t sz = rd32(ck + 4);
    const size_t body = o + 8, avail = std::min<size_t>(sz, n - body);
    if (std::memcmp(ck, "fmt ", 4) == 0 && avail >= 16) {
      tag = rd16(ck + 8);
      chans = rd16(ck + 10);
      rate = rd32(ck + 12);
      bits = rd16(ck + 22);
      if (tag == 0xFFFE && avail >= 26) tag = rd16(ck + 8 + 24);
    } else if (std::memcmp(ck, "data", 4) == 0) {
      data = p + body;
      dataLen = avail;
    } else if (std::memcmp(ck, "cue ", 4) == 0 && avail >= 4) {
      uint32_t cnt = rd32(p + body);
      for (uint32_t k = 0; k < cnt && 4 + 24 * (k + 1) <= avail; ++k) {
        const uint8_t* q = p + body + 4 + 24 * k;
        cues.push_back({rd32(q), rd32(q + 20)});
      }
    } else if (std::memcmp(ck, "LIST", 4) == 0 && avail >= 4 && std::memcmp(p + body, "adtl", 4) == 0) {
      size_t q = body + 4;
      while (q + 8 <= body + avail) {
        uint32_t s2 = rd32(p + q + 4);
        if (q + 8 + s2 > body + avail) break;
        if ((std::memcmp(p + q, "labl", 4) == 0 || std::memcmp(p + q, "note", 4) == 0) && s2 >= 4) {
          std::string name(reinterpret_cast<const char*>(p + q + 12), s2 - 4);
          name = name.c_str();
          if (std::memcmp(p + q, "labl", 4) == 0) labels[rd32(p + q + 8)] = name;
        } else if (std::memcmp(p + q, "ltxt", 4) == 0 && s2 >= 8) {
          lengths[rd32(p + q + 8)] = rd32(p + q + 12);
        }
        q += 8 + s2 + (s2 & 1);
      }
    } else if (std::memcmp(ck, "smpl", 4) == 0 && avail >= 36) {
      uint32_t nl = rd32(p + body + 28);
      if (nl > 0 && avail >= 36 + 24) {
        loopS = rd32(p + body + 36 + 8);
        loopE = static_cast<int64_t>(rd32(p + body + 36 + 12)) + 1;
      }
    }
    o = body + sz + (sz & 1);
  }
  const bool flt = tag == 3;
  if (!data || chans == 0 || rate == 0 || (tag != 1 && tag != 3) || (flt && bits != 32 && bits != 64) ||
      (!flt && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
    if (err) *err = "unsupported WAV format";
    return false;
  }
  const size_t bps = bits / 8, align = bps * chans;
  const int64_t frames = static_cast<int64_t>(dataLen / align);
  Audio a = makeAudio(rate, chans == 1 ? 1 : 2, frames);
  for (int64_t i = 0; i < frames; ++i)
    for (uint32_t ch = 0; ch < std::min<uint32_t>(chans, 2); ++ch) {
      const uint8_t* s = data + static_cast<size_t>(i) * align + ch * bps;
      double v = 0;
      if (flt && bits == 32) {
        float f;
        std::memcpy(&f, s, 4);
        v = f;
      } else if (flt) {
        double d;
        std::memcpy(&d, s, 8);
        v = d;
      } else if (bits == 8) {
        v = (static_cast<int>(s[0]) - 128) / 128.0;
      } else if (bits == 16) {
        v = static_cast<int16_t>(rd16(s)) / 32768.0;
      } else if (bits == 24) {
        int32_t q = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (static_cast<uint32_t>(s[2]) << 24)) >> 8;
        v = q / 8388608.0;
      } else {
        v = static_cast<int32_t>(rd32(s)) / 2147483648.0;
      }
      a.ch(static_cast<int>(ch))[i] = static_cast<float>(v);
    }
  fixMono(a);
  out = makeContent(std::move(a));
  for (const auto& q : cues) {
    std::string name = labels.count(q.id) ? labels[q.id] : std::string();
    if (lengths.count(q.id) && lengths[q.id] > 0)
      out.regions.push_back({q.pos, static_cast<int64_t>(q.pos) + lengths[q.id], name});
    else
      out.markers.push_back({q.pos, name});
  }
  std::stable_sort(out.markers.begin(), out.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
  std::stable_sort(out.regions.begin(), out.regions.end(), [](const Region& x, const Region& y) { return x.start < y.start; });
  if (loopE > loopS && loopS >= 0 && loopE <= frames) out.loopStart = loopS, out.loopEnd = loopE;
  return true;
}

bool loadContent(const std::string& path, Content& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && parseWav(bytes.data(), bytes.size(), out, nullptr)) return true;
  SamplePtr s = loadSample(path, err);
  if (!s) return false;
  out = makeContent(fromSample(*s));
  return true;
}

// ------------------------------------------------------------------ FLAC encoder
namespace {

class BitWriter {
 public:
  void put(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bit(static_cast<unsigned>((v >> i) & 1u));
  }
  void bit(unsigned b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | b);
    if (++fill_ == 8) bytes.push_back(cur_), cur_ = 0, fill_ = 0;
  }
  void zeros(uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) bit(0);
  }
  void align() {
    while (fill_ != 0) bit(0);
  }
  std::vector<uint8_t> bytes;

 private:
  uint8_t cur_ = 0;
  int fill_ = 0;
};

uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = static_cast<uint8_t>((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
  }
  return c;
}

uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c ^= static_cast<uint16_t>(p[i] << 8);
    for (int k = 0; k < 8; ++k) c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1));
  }
  return c;
}

void residuals(const int32_t* x, int n, int order, std::vector<int64_t>& e) {
  e.clear();
  for (int i = order; i < n; ++i) {
    int64_t v = x[i];
    switch (order) {
      case 1: v -= x[i - 1]; break;
      case 2: v -= 2LL * x[i - 1] - x[i - 2]; break;
      case 3: v -= 3LL * x[i - 1] - 3LL * x[i - 2] + x[i - 3]; break;
      case 4: v -= 4LL * x[i - 1] - 6LL * x[i - 2] + 4LL * x[i - 3] - x[i - 4]; break;
      default: break;
    }
    e.push_back(v);
  }
}

uint64_t riceBits(const std::vector<int64_t>& e, int k) {
  uint64_t bits = 0;
  for (int64_t v : e) {
    uint64_t u = v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(-v) << 1) - 1;
    bits += (u >> k) + 1 + static_cast<uint64_t>(k);
  }
  return bits;
}

void subframe(BitWriter& w, const int32_t* x, int n, int bps) {
  bool constant = true;
  for (int i = 1; i < n && constant; ++i) constant = x[i] == x[0];
  if (constant) {
    w.put(0, 1);
    w.put(0, 6);
    w.put(0, 1);
    w.put(static_cast<uint64_t>(static_cast<uint32_t>(x[0])) & ((1ull << bps) - 1), bps);
    return;
  }
  std::vector<int64_t> e, bestE;
  int bestOrder = -1, bestK = 0;
  uint64_t bestBits = static_cast<uint64_t>(bps) * static_cast<uint64_t>(n);  // verbatim cost
  for (int order = 0; order <= std::min(4, n - 1); ++order) {
    residuals(x, n, order, e);
    bool fits = true;
    double mean = 0;
    for (int64_t v : e) {
      if (v > INT32_MAX || v < INT32_MIN) fits = false;
      mean += static_cast<double>(v < 0 ? -v : v);
    }
    if (!fits) continue;
    mean = e.empty() ? 0 : 2.0 * mean / static_cast<double>(e.size());
    int k0 = mean > 1 ? static_cast<int>(std::log2(mean)) : 0;
    for (int k = std::max(0, k0 - 1); k <= std::min(30, k0 + 1); ++k) {
      uint64_t bits = riceBits(e, k) + static_cast<uint64_t>(order) * static_cast<uint64_t>(bps) + 2 + 4 + 5;
      if (bits < bestBits) bestBits = bits, bestOrder = order, bestK = k, bestE = e;
    }
  }
  w.put(0, 1);
  if (bestOrder < 0) {  // verbatim
    w.put(1, 6);
    w.put(0, 1);
    for (int i = 0; i < n; ++i) w.put(static_cast<uint64_t>(static_cast<uint32_t>(x[i])) & ((1ull << bps) - 1), bps);
    return;
  }
  w.put(static_cast<uint64_t>(8 | bestOrder), 6);  // 001xxx: fixed predictor
  w.put(0, 1);
  for (int i = 0; i < bestOrder; ++i) w.put(static_cast<uint64_t>(static_cast<uint32_t>(x[i])) & ((1ull << bps) - 1), bps);
  w.put(1, 2);  // Rice coding with 5-bit parameters
  w.put(0, 4);  // partition order 0
  w.put(static_cast<uint64_t>(bestK), 5);
  for (int64_t v : bestE) {
    uint64_t u = v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(-v) << 1) - 1;
    w.zeros(u >> bestK);
    w.bit(1);
    if (bestK) w.put(u & ((1ull << bestK) - 1), bestK);
  }
}

void utf8Number(std::vector<uint8_t>& b, uint64_t v) {
  if (v < 0x80) {
    b.push_back(static_cast<uint8_t>(v));
    return;
  }
  int n = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : 6;
  b.push_back(static_cast<uint8_t>((0xFF00 >> n) | (v >> (6 * (n - 1)))));
  for (int i = n - 2; i >= 0; --i) b.push_back(static_cast<uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F)));
}

}  // namespace

bool writeFlacFile(const std::string& path, const Audio& a, int bits, std::string* err) {
  if (bits != 16 && bits != 24) bits = 24;
  const int chans = a.channels == 1 ? 1 : 2, block = 4096;
  const uint32_t rate = static_cast<uint32_t>(std::lround(a.rate));
  if (rate == 0 || rate >= (1u << 20)) {
    if (err) *err = "sample rate not supported by FLAC";
    return false;
  }
  std::vector<uint8_t> out = {'f', 'L', 'a', 'C'};
  {
    BitWriter w;
    w.put(1, 1);  // last metadata block
    w.put(0, 7);  // STREAMINFO
    w.put(34, 24);
    w.put(static_cast<uint64_t>(block), 16);
    w.put(static_cast<uint64_t>(block), 16);
    w.put(0, 24);
    w.put(0, 24);
    w.put(rate, 20);
    w.put(static_cast<uint64_t>(chans - 1), 3);
    w.put(static_cast<uint64_t>(bits - 1), 5);
    w.put(static_cast<uint64_t>(a.frames()), 36);
    for (int i = 0; i < 16; ++i) w.put(0, 8);  // MD5 unknown
    out.insert(out.end(), w.bytes.begin(), w.bytes.end());
  }
  std::vector<int32_t> buf(static_cast<size_t>(block));
  uint64_t frameNo = 0;
  for (int64_t s = 0; s < a.frames(); s += block, ++frameNo) {
    const int n = static_cast<int>(std::min<int64_t>(block, a.frames() - s));
    std::vector<uint8_t> f = {0xFF, 0xF8};
    f.push_back(static_cast<uint8_t>(0x70));  // block size: 16-bit field at the end; rate from STREAMINFO
    f.push_back(static_cast<uint8_t>(((chans - 1) << 4) | ((bits == 16 ? 4 : 6) << 1)));
    utf8Number(f, frameNo);
    f.push_back(static_cast<uint8_t>((n - 1) >> 8));
    f.push_back(static_cast<uint8_t>(n - 1));
    f.push_back(crc8(f.data(), f.size()));
    BitWriter w;
    for (int ch = 0; ch < chans; ++ch) {
      for (int i = 0; i < n; ++i) buf[static_cast<size_t>(i)] = quantize(a.ch(ch)[s + i], bits);
      subframe(w, buf.data(), n, bits);
    }
    w.align();
    f.insert(f.end(), w.bytes.begin(), w.bytes.end());
    uint16_t c = crc16(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(c >> 8));
    f.push_back(static_cast<uint8_t>(c));
    out.insert(out.end(), f.begin(), f.end());
  }
  return writeBytes(path, out, err);
}

bool exportFile(const std::string& path, const Content& c, Range r, FileFormat f, int bits, std::string* err) {
  Range rr = rangeOrAll(c, r);
  Content part = c;
  if (rr.a > 0 || rr.b < c.frames()) part = cropTo(c, rr);
  if (f == FileFormat::Flac) return writeFlacFile(path, part.a(), bits, err);
  return writeWavFile(path, part, bits, err);
}

// ------------------------------------------------------------------ hashing / paths
uint64_t audioHash(const Audio& a) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(&a.rate, sizeof a.rate);
  mix(&a.channels, sizeof a.channels);
  mix(a.L.data(), a.L.size() * sizeof(float));
  if (a.channels == 2) mix(a.R.data(), a.R.size() * sizeof(float));
  return h;
}

std::string hashHex(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

std::string renderPath(const std::string& dir, const std::string& name, uint64_t hash) {
  std::string clean;
  for (char ch : name) clean += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') ? ch : '_';
  if (clean.empty()) clean = "edison";
  return (fs::path(dir) / (clean.substr(0, 40) + "-" + hashHex(hash).substr(0, 12) + ".wav")).string();
}

// ------------------------------------------------------------------ plugin state
std::string encodeState(const State& s) {
  Json j = Json::makeObject();
  j.object["v"] = Json::makeNumber(1);
  if (!s.path.empty()) j.object["path"] = Json::makeString(s.path);
  if (!s.hash.empty()) j.object["hash"] = Json::makeString(s.hash);
  if (!s.name.empty()) j.object["name"] = Json::makeString(s.name);
  if (!s.markers.empty()) {
    Json arr = Json::makeArray();
    for (const auto& m : s.markers) {
      Json o = Json::makeObject();
      o.object["pos"] = Json::makeNumber(static_cast<double>(m.pos));
      o.object["name"] = Json::makeString(m.name);
      arr.array.push_back(o);
    }
    j.object["markers"] = arr;
  }
  if (!s.regions.empty()) {
    Json arr = Json::makeArray();
    for (const auto& g : s.regions) {
      Json o = Json::makeObject();
      o.object["start"] = Json::makeNumber(static_cast<double>(g.start));
      o.object["end"] = Json::makeNumber(static_cast<double>(g.end));
      o.object["name"] = Json::makeString(g.name);
      arr.array.push_back(o);
    }
    j.object["regions"] = arr;
  }
  if (s.loopEnd > s.loopStart && s.loopStart >= 0) {
    j.object["loopStart"] = Json::makeNumber(static_cast<double>(s.loopStart));
    j.object["loopEnd"] = Json::makeNumber(static_cast<double>(s.loopEnd));
  }
  if (s.flVersion) j.object["flVersion"] = Json::makeNumber(s.flVersion);
  if (!s.flExtra.empty()) j.object["flExtra"] = Json::makeString(s.flExtra);
  return jsonStringify(j, 0);
}

bool decodeState(const std::string& data, State& s) {
  s = State{};
  if (data.empty()) return true;
  Json j;
  if (data[0] != '{' || !jsonParse(data, j) || !j.is(Json::Type::Object)) {
    s.path = data;  // a plain sample path (e.g. typed by hand)
    return true;
  }
  s.path = j.text("path");
  s.hash = j.text("hash");
  s.name = j.text("name");
  if (const Json* m = j.find("markers"))
    for (const auto& o : m->array) s.markers.push_back({static_cast<int64_t>(o.num("pos")), o.text("name")});
  if (const Json* r = j.find("regions"))
    for (const auto& o : r->array) s.regions.push_back({static_cast<int64_t>(o.num("start")), static_cast<int64_t>(o.num("end")), o.text("name")});
  s.loopStart = static_cast<int64_t>(j.num("loopStart", -1));
  s.loopEnd = static_cast<int64_t>(j.num("loopEnd", -1));
  s.flVersion = static_cast<int>(j.num("flVersion", 0));
  s.flExtra = j.text("flExtra");
  return true;
}

// ------------------------------------------------------------------ FL Studio Edison state
// Layout (FL 2026's shipped "Edison" presets, versions 4 and 6):
//   i32 version; (version 4: 2, version 6: 3) i32 header fields;
//   u32 size + RIFF/WAVE image of the loaded audio (markers as cue / labl /
//   ltxt chunks, loop points in "smpl"); editor records (envelope / LFO
//   panels, ~80 bytes each); then version − 1 trailing i32 settings:
//   [0] record mode (0 now, 1 on input, 2 input, 3 on play), [2] maximum
//   recording time (index into the time list), [4] (v6) slave to host playback.
bool decodeFlState(const uint8_t* p, size_t n, FlEdison& out) {
  out = FlEdison{};
  if (n < 12) return false;
  out.version = static_cast<int32_t>(rd32(p));
  if (out.version < 1 || out.version > 64) return false;
  size_t riff = 0;
  for (size_t o = 4; o + 12 <= n && o < 96; o += 4)
    if (std::memcmp(p + o, "RIFF", 4) == 0 && std::memcmp(p + o + 8, "WAVE", 4) == 0) {
      riff = o;
      break;
    }
  if (riff < 8) return false;
  for (size_t o = 4; o + 4 <= riff - 4; o += 4) out.head.push_back(static_cast<int32_t>(rd32(p + o)));
  const uint32_t len = rd32(p + riff - 4);
  if (riff + len > n) return false;
  Content c;
  if (parseWav(p + riff, len, c, nullptr)) {
    out.hasAudio = c.frames() > 0;
    out.content = std::move(c);
  }
  const size_t tail = riff + len;
  const size_t nSet = std::min<size_t>(static_cast<size_t>(std::max(0, out.version - 1)), (n - tail) / 4);
  const size_t setAt = n - nSet * 4;
  out.extra.assign(p + tail, p + setAt);
  for (size_t o = setAt; o + 4 <= n; o += 4) out.settings.push_back(static_cast<int32_t>(rd32(p + o)));
  if (!out.settings.empty()) out.recMode = std::clamp(out.settings[0], 0, 3);
  if (out.settings.size() > 2) out.recMaxChoice = out.settings[2];
  if (out.settings.size() > 4) out.followHost = out.settings[4] != 0;
  return true;
}

bool mapFlState(const uint8_t* p, size_t n, const std::string& audioDir, PluginState& out, std::string* note) {
  const PluginInfo* info = findPlugin(kType);
  out = info ? defaultPluginState(kType) : PluginState{};
  out.type = kType;
  out.params.resize(EdCount, 0.f);
  FlEdison fl;
  if (!decodeFlState(p, n, fl)) {
    if (note) *note = "Edison: state not recognised (defaults kept)";
    State s;
    s.flExtra = toHex(p, std::min<size_t>(n, 4096));
    out.data = encodeState(s);
    return false;
  }
  out.params[EdRecMode] = static_cast<float>(fl.recMode);
  static const int kMax[] = {0, 0, 1, 2, 3, 4, 5, 6};  // FL time list (with a separator) → Gridline choice
  out.params[EdRecMax] = static_cast<float>(fl.recMaxChoice >= 0 && fl.recMaxChoice < 8 ? kMax[fl.recMaxChoice] : 0);
  out.params[EdFollowHost] = fl.followHost ? 1.f : 0.f;
  State s;
  s.flVersion = fl.version;
  std::vector<uint8_t> keep;
  for (int32_t v : fl.head) put32(keep, static_cast<uint32_t>(v));
  s.flExtra = toHex(keep.data(), keep.size()) + ":" + toHex(fl.extra.data(), fl.extra.size()) + ":";
  keep.clear();
  for (int32_t v : fl.settings) put32(keep, static_cast<uint32_t>(v));
  s.flExtra += toHex(keep.data(), keep.size());
  if (fl.hasAudio) {
    const uint64_t h = audioHash(fl.content.a());
    const std::string path = renderPath(audioDir, "fl-edison", h);
    std::error_code ec;
    if (fs::is_regular_file(path, ec) || writeWavFile(path, fl.content, 32, nullptr)) {
      s.path = path;
      s.hash = hashHex(h);
      s.name = "Imported audio";
      s.markers = fl.content.markers;
      s.regions = fl.content.regions;
      s.loopStart = fl.content.loopStart;
      s.loopEnd = fl.content.loopEnd;
    }
    if (note) {
      char b[128];
      std::snprintf(b, sizeof b, "Edison: %.1f s of audio imported into the audio editor", fl.content.a().seconds());
      *note = b;
    }
  } else if (note) {
    *note = "Edison: empty editor imported (recording settings kept)";
  }
  out.data = encodeState(s);
  return true;
}

}  // namespace gl::packs::edison

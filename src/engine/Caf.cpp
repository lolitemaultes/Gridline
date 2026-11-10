// Gridline - Core Audio Format files (.caf). See Caf.h.
//
// Container: "caff", version 1, then chunks of a 4-byte type and a signed
// 64-bit big-endian size (-1 for an audio data chunk that runs to the end of
// the file). Chunks read here: desc (format), data (audio, after a 32-bit
// edit count), pakt (packet table), kuki (codec cookie), info (text pairs)
// and two uuid chunks of tagged loops: the loop metadata (a count, then
// NUL-terminated key and value strings) and the beat markers (a 20-byte
// header whose last word is the count, then 12-byte entries with the frame
// in their last word).
#include "Caf.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../core/Platform.h"

namespace gl::caf {

namespace fs = std::filesystem;

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
double beDouble(const uint8_t* p) {
  const uint64_t b = be64(p);
  double d;
  std::memcpy(&d, &b, sizeof d);
  return d;
}

bool fail(std::string* err, const char* why) {
  if (err) *err = why;
  return false;
}

constexpr uint8_t kLoopUuid[16] = {0x29, 0x81, 0x92, 0x73, 0xb5, 0xbf, 0x4a, 0xef, 0xb7, 0x8d, 0x62, 0xd1, 0xef, 0x90, 0xbb, 0x2c};
constexpr uint8_t kMarkerUuid[16] = {0x03, 0x52, 0x81, 0x1b, 0x9d, 0x5d, 0x42, 0xe1, 0x88, 0x2d, 0x6a, 0xf6, 0x1a, 0x6b, 0x33, 0x0c};

// A count, then NUL-terminated key / value strings.
void readPairs(const uint8_t* d, size_t n, std::vector<std::pair<std::string, std::string>>& out) {
  if (n < 4) return;
  const uint32_t count = be32(d);
  size_t at = 4;
  auto str = [&](std::string& s) {
    const void* z = at < n ? std::memchr(d + at, 0, n - at) : nullptr;
    if (!z) return false;
    const size_t end = static_cast<size_t>(static_cast<const uint8_t*>(z) - d);
    s.assign(reinterpret_cast<const char*>(d + at), end - at);
    at = end + 1;
    return true;
  };
  for (uint32_t k = 0; k < count && k < 256; ++k) {
    std::string a, b;
    if (!str(a) || !str(b)) break;
    out.emplace_back(a, b);
  }
}

// Packet table entries: big-endian base-128 numbers (high bit = more bytes follow).
bool readVarint(const uint8_t* d, size_t n, size_t& at, uint64_t& v) {
  v = 0;
  for (int k = 0; k < 10; ++k) {
    if (at >= n) return false;
    const uint8_t b = d[at++];
    v = (v << 7) | (b & 0x7Fu);
    if (!(b & 0x80u)) return true;
  }
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// "C", "F#", "Bb" -> pitch class; -1 when not a note name.
int noteClass(const std::string& text) {
  const std::string s = trim(text);
  if (s.empty()) return -1;
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
  const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  if (c < 'A' || c > 'G') return -1;
  int pc = base[c - 'A'];
  if (s.size() >= 2) {
    if (s[1] == '#') pc += 1;
    else if (s[1] == 'b') pc -= 1;
    else return -1;
    if (s.size() > 2) return -1;
  }
  return (pc + 12) % 12;
}

}  // namespace

int64_t Info::frames() const {
  if (hasPakt && validFrames > 0) return validFrames;
  if (lpcm() && bytesPerPacket > 0 && hasData) return static_cast<int64_t>(dataSize / bytesPerPacket) * std::max<uint32_t>(1, framesPerPacket);
  if (hasPakt && packets > 0 && framesPerPacket > 0) return packets * framesPerPacket - priming - remainder;
  return 0;
}

std::string Info::value(const std::string& key) const {
  for (const auto& [k, v] : meta)
    if (lower(k) == key) return v;
  return "";
}

bool looksLikeCaf(const uint8_t* p, size_t n) { return n >= 8 && std::memcmp(p, "caff", 4) == 0 && p[4] == 0 && p[5] == 1; }

bool parse(const uint8_t* p, size_t n, Info& out, std::string* err) {
  out = Info{};
  if (!looksLikeCaf(p, n)) return fail(err, "not a CAF file");
  bool desc = false;
  size_t at = 8;
  for (int guard = 0; at + 12 <= n && guard < 4096; ++guard) {
    const uint8_t* c = p + at;
    const int64_t size = static_cast<int64_t>(be64(c + 4));
    const size_t body = at + 12;
    size_t len = 0;
    if (size < 0) {
      if (std::memcmp(c, "data", 4) != 0) return fail(err, "damaged chunk list");
      len = n - body;  // the data chunk runs to the end of the file
    } else {
      if (static_cast<uint64_t>(size) > n - body) {
        // A truncated last chunk: data keeps what is there, anything else ends the list.
        if (std::memcmp(c, "data", 4) != 0) break;
        len = n - body;
      } else {
        len = static_cast<size_t>(size);
      }
    }
    const uint8_t* d = p + body;
    if (std::memcmp(c, "desc", 4) == 0 && len >= 32) {
      out.sampleRate = beDouble(d);
      std::memcpy(out.format, d + 8, 4);
      out.formatFlags = be32(d + 12);
      out.bytesPerPacket = be32(d + 16);
      out.framesPerPacket = be32(d + 20);
      out.channels = be32(d + 24);
      out.bits = be32(d + 28);
      desc = true;
    } else if (std::memcmp(c, "data", 4) == 0 && len >= 4) {
      out.dataOffset = body + 4;
      out.dataSize = len - 4;
      out.hasData = true;
    } else if (std::memcmp(c, "pakt", 4) == 0 && len >= 24) {
      out.hasPakt = true;
      out.packets = static_cast<int64_t>(be64(d));
      out.validFrames = static_cast<int64_t>(be64(d + 8));
      out.priming = static_cast<int32_t>(be32(d + 16));
      out.remainder = static_cast<int32_t>(be32(d + 20));
      size_t q = 24;
      for (int64_t k = 0; k < out.packets && k < (int64_t{1} << 24); ++k) {
        uint64_t v = 0;
        if (!readVarint(d, len, q, v)) break;
        out.packetSizes.push_back(static_cast<uint32_t>(std::min<uint64_t>(v, 0xFFFFFFFFu)));
        if (desc && out.framesPerPacket == 0 && !readVarint(d, len, q, v)) break;  // per-packet frame counts: skipped
      }
    } else if (std::memcmp(c, "kuki", 4) == 0) {
      out.cookie.assign(d, d + std::min<size_t>(len, 1u << 16));
    } else if (std::memcmp(c, "info", 4) == 0) {
      readPairs(d, len, out.info);
    } else if (std::memcmp(c, "uuid", 4) == 0 && len >= 16) {
      if (std::memcmp(d, kLoopUuid, 16) == 0) {
        out.hasLoop = true;
        readPairs(d + 16, len - 16, out.meta);
      } else if (std::memcmp(d, kMarkerUuid, 16) == 0 && len >= 36) {
        const uint8_t* m = d + 16;
        const uint32_t count = be32(m + 16);
        for (uint32_t k = 0; k < count && 20 + static_cast<size_t>(k) * 12 + 12 <= len - 16 && k < 100000; ++k)
          out.markers.push_back(be32(m + 20 + static_cast<size_t>(k) * 12 + 8));
      }
    }
    if (size < 0) break;
    at = body + len;
  }
  if (!desc) return fail(err, "missing audio format");
  if (!(out.sampleRate >= 1000 && out.sampleRate <= 768000) || out.channels == 0 || out.channels > 16) return fail(err, "bad audio format");
  if (!out.hasData) return fail(err, "missing audio");
  return true;
}

namespace {

// Linear PCM: integers (signed, 8 to 32 bits) or floats (32 / 64 bits), either byte order.
bool decodeLpcm(const uint8_t* p, const Info& in, DecodedAudio& out, std::string* err) {
  const bool isFloat = (in.formatFlags & 1u) != 0, little = (in.formatFlags & 2u) != 0;
  const uint32_t ch = in.channels, bits = in.bits;
  if (isFloat ? (bits != 32 && bits != 64) : (bits < 8 || bits > 32)) return fail(err, "unsupported sample format");
  const uint32_t bytes = (bits + 7) / 8;
  const uint32_t frameBytes = in.bytesPerPacket > 0 ? in.bytesPerPacket : bytes * ch;
  if (frameBytes < bytes * ch) return fail(err, "bad audio format");
  int64_t frames = static_cast<int64_t>(in.dataSize / frameBytes);
  if (in.hasPakt && in.validFrames > 0) frames = std::min(frames, in.validFrames);
  if (frames <= 0) return fail(err, "no audio in the file");
  out.channels = static_cast<int>(ch);
  out.sampleRate = static_cast<int>(std::lround(in.sampleRate));
  out.bitsPerSample = static_cast<int>(bits);
  out.interleaved.resize(static_cast<size_t>(frames) * ch);
  const uint8_t* d = p + in.dataOffset;
  for (int64_t f = 0; f < frames; ++f) {
    if ((f & 0xFFFF) == 0 && decodeCancelled()) return fail(err, "cancelled");
    for (uint32_t c = 0; c < ch; ++c) {
      const uint8_t* s = d + static_cast<size_t>(f) * frameBytes + static_cast<size_t>(c) * bytes;
      uint64_t raw = 0;
      for (uint32_t k = 0; k < bytes; ++k) raw |= static_cast<uint64_t>(s[little ? k : bytes - 1 - k]) << (8 * k);
      float v = 0;
      if (isFloat) {
        if (bits == 32) {
          const uint32_t r32 = static_cast<uint32_t>(raw);
          float x;
          std::memcpy(&x, &r32, sizeof x);
          v = x;
        } else {
          double x;
          std::memcpy(&x, &raw, sizeof x);
          v = static_cast<float>(x);
        }
        if (!std::isfinite(v)) v = 0;
      } else {
        // Samples sit in the high bits of their bytes when `bits` is not a whole number of bytes.
        const int total = static_cast<int>(bytes * 8);
        int64_t sv = static_cast<int64_t>(raw << (64 - total)) >> (64 - total);
        v = static_cast<float>(static_cast<double>(sv) / static_cast<double>(int64_t{1} << (total - 1)));
      }
      out.interleaved[static_cast<size_t>(f) * ch + c] = v;
    }
  }
  return true;
}

// The cookie's audio configuration (MPEG-4 elementary stream descriptor, or the bare configuration).
bool audioConfig(const std::vector<uint8_t>& ck, int& objectType, int& freqIndex, int& chanConfig) {
  const uint8_t* asc = nullptr;
  size_t ascLen = 0;
  // Descriptor walk: tag, length (up to four 7-bit bytes), body. 0x03 holds 0x04 holds 0x05 (the configuration).
  size_t at = 0;
  const size_t n = ck.size();
  for (int guard = 0; at + 2 <= n && guard < 32; ++guard) {
    const uint8_t tag = ck[at++];
    size_t len = 0;
    for (int k = 0; k < 4 && at < n; ++k) {
      const uint8_t b = ck[at++];
      len = (len << 7) | (b & 0x7Fu);
      if (!(b & 0x80u)) break;
    }
    if (tag == 0x03) {
      if (at + 3 > n) return false;
      const uint8_t flags = ck[at + 2];
      at += 3;
      if (flags & 0x80) at += 2;
      if (flags & 0x40) {
        if (at >= n) return false;
        at += 1 + ck[at];
      }
      if (flags & 0x20) at += 2;
      continue;
    }
    if (tag == 0x04) {
      at += 13;
      continue;
    }
    if (tag == 0x05) {
      asc = ck.data() + at;
      ascLen = std::min(len, n - std::min(n, at));
      break;
    }
    at += len;
  }
  if (!asc && n >= 2 && n <= 64) asc = ck.data(), ascLen = n;  // a bare configuration
  if (!asc || ascLen < 2) return false;
  // Bit reader over the configuration.
  size_t bit = 0;
  auto read = [&](int count) {
    uint32_t v = 0;
    for (int k = 0; k < count; ++k, ++bit) {
      const size_t byte = bit / 8;
      const uint32_t b = byte < ascLen ? (asc[byte] >> (7 - bit % 8)) & 1u : 0u;
      v = (v << 1) | b;
    }
    return v;
  };
  objectType = static_cast<int>(read(5));
  if (objectType == 31) objectType = 32 + static_cast<int>(read(6));
  freqIndex = static_cast<int>(read(4));
  if (freqIndex == 15) return false;  // an explicit rate: ADTS cannot carry it
  chanConfig = static_cast<int>(read(4));
  if (objectType == 5 || objectType == 29) {  // spectral band replication: the stream carries the core layer
    if (read(4) == 15) return false;  // the output rate; ADTS states the core rate read above
    objectType = static_cast<int>(read(5));
  }
  return objectType >= 1 && objectType <= 4 && chanConfig >= 1 && chanConfig <= 7;
}

std::atomic<unsigned> gTemp{0};

}  // namespace

std::vector<uint8_t> toAdts(const uint8_t* p, size_t n, const Info& in) {
  std::vector<uint8_t> out;
  int ot = 0, fi = 0, cc = 0;
  if (!in.aac() || !in.hasData || in.packetSizes.empty() || !audioConfig(in.cookie, ot, fi, cc)) return out;
  size_t at = in.dataOffset;
  const size_t end = std::min(n, in.dataOffset + in.dataSize);
  for (uint32_t sz : in.packetSizes) {
    if (sz == 0 || sz > 8191 - 7 || at + sz > end) break;
    const uint32_t len = sz + 7;
    const uint8_t h[7] = {0xFF, 0xF1, static_cast<uint8_t>(((ot - 1) << 6) | (fi << 2) | (cc >> 2)),
                          static_cast<uint8_t>(((cc & 3) << 6) | (len >> 11)), static_cast<uint8_t>((len >> 3) & 0xFF),
                          static_cast<uint8_t>(((len & 7) << 5) | 0x1F), 0xFC};
    out.insert(out.end(), h, h + 7);
    out.insert(out.end(), p + at, p + at + sz);
    at += sz;
  }
  return out;
}

void trimToPacketTable(DecodedAudio& d, const Info& in) {
  if (!in.hasPakt || in.validFrames <= 0 || d.channels <= 0) return;
  const int64_t have = static_cast<int64_t>(d.frames());
  const int64_t fpp = in.framesPerPacket > 0 ? in.framesPerPacket : 1024;
  const int64_t raw = in.packets * fpp;
  // Output samples per stated frame (2 when the decoder doubled the rate of a band-replicated stream).
  const double scale = in.sampleRate > 0 && d.sampleRate > 0 ? d.sampleRate / in.sampleRate : 1.0;
  const int64_t valid = static_cast<int64_t>(std::llround(static_cast<double>(in.validFrames) * scale));
  const int64_t prime = static_cast<int64_t>(std::llround(static_cast<double>(in.priming) * scale));
  int64_t start = 0;
  // Raw output still holds the priming: it is about the packets' length.
  if (std::llabs(have - static_cast<int64_t>(std::llround(static_cast<double>(raw) * scale))) <= fpp && prime > 0 && prime < have) start = prime;
  const int64_t keep = std::min(valid, have - start);
  if (keep <= 0) return;
  const size_t ch = static_cast<size_t>(d.channels);
  if (start > 0) d.interleaved.erase(d.interleaved.begin(), d.interleaved.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(start) * ch));
  d.interleaved.resize(static_cast<size_t>(keep) * ch);
}

bool decode(const std::string& path, const uint8_t* p, size_t n, DecodedAudio& out, std::string* err) {
  out = DecodedAudio{};
  Info in;
  if (!parse(p, n, in, err)) return false;
  if (in.lpcm()) return decodeLpcm(p, in, out, err);
  // Compressed: the computer's decoder (or FFmpeg) reads the container itself.
  std::string why;
  if (decodeAacFile(path, out, &why)) {
    trimToPacketTable(out, in);
    return true;
  }
  if (decodeCancelled()) return fail(err, "cancelled");
  // AAC for a decoder that cannot open the container: the packets as a raw AAC stream.
  if (in.aac()) {
    const std::vector<uint8_t> adts = toAdts(p, n, in);
    if (!adts.empty()) {
      std::error_code ec;
      fs::path dir = fs::temp_directory_path(ec);
      if (ec || dir.empty()) dir = fs::u8path(path).parent_path();
      const fs::path tmp = dir / ("gridline-caf-" + std::to_string(platform::processId()) + "-" + std::to_string(gTemp.fetch_add(1)) + ".aac");
      {
        std::ofstream os(tmp, std::ios::binary);
        os.write(reinterpret_cast<const char*>(adts.data()), static_cast<std::streamsize>(adts.size()));
      }
      out = DecodedAudio{};
      const bool ok = decodeAacFile(tmp.u8string(), out, &why);
      fs::remove(tmp, ec);
      if (ok) {
        trimToPacketTable(out, in);
        return true;
      }
    }
  }
  out = DecodedAudio{};
  if (err) *err = why.empty() ? std::string("no decoder for this audio format") : why;
  return false;
}

Loop loopOf(const Info& in) {
  Loop l;
  const std::string beats = trim(in.value("beat count"));
  if (!beats.empty()) {
    char* end = nullptr;
    const double b = std::strtod(beats.c_str(), &end);
    if (end != beats.c_str() && std::isfinite(b) && b > 0 && b < 100000) l.beats = b;
  }
  const std::string sig = trim(in.value("time signature"));
  if (const size_t slash = sig.find('/'); slash != std::string::npos) {
    const int a = std::atoi(sig.substr(0, slash).c_str()), b = std::atoi(sig.substr(slash + 1).c_str());
    if (a > 0 && a <= 64 && (b == 1 || b == 2 || b == 4 || b == 8 || b == 16 || b == 32)) l.sigNum = a, l.sigDen = b;
  }
  l.key = noteClass(in.value("key signature"));
  l.minor = lower(trim(in.value("key type"))) == "minor";
  auto add = [&l](const std::string& t) {
    const std::string s = lower(trim(t));
    if (!s.empty() && s.size() <= 40 && std::find(l.tags.begin(), l.tags.end(), s) == l.tags.end()) l.tags.push_back(s);
  };
  for (const char* k : {"category", "subcategory", "genre"}) add(in.value(k));
  const std::string desc = in.value("descriptors");
  size_t a = 0;
  while (a <= desc.size()) {
    const size_t b = desc.find(',', a);
    add(desc.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  for (const auto& [k, v] : in.info)
    if (lower(k) == "genre") add(v);
  return l;
}

}  // namespace gl::caf

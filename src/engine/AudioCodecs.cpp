// Gridline — Ogg Vorbis decoding through the vendored stb_vorbis (public
// domain, third_party/stb). Kept in its own translation unit: stb_vorbis
// defines short macro names.
#include <climits>
#include <cstring>
#include <memory>
#include <vector>
#include "WavPack.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb/stb_vorbis.c>

namespace gl {

namespace {

// Decodes one logical (unchained) Ogg Vorbis stream.
bool decodeOne(const uint8_t* data, size_t size, DecodedAudio& out, std::string* err) {
  out = DecodedAudio{};
  if (size > static_cast<size_t>(INT_MAX)) {
    if (err) *err = "Ogg stream too large";
    return false;
  }
  int e = 0;
  // All of stb_vorbis's setup / temp memory comes from one bounded arena: a
  // corrupt header (e.g. a comment count of 2^31) then fails with "out of
  // memory" at once instead of allocating per comment for minutes. Real
  // streams need well under 1 MB; pages are only touched when used.
  constexpr int kArena = 32 << 20;
  std::unique_ptr<char[]> arena(new char[kArena]);
  stb_vorbis_alloc alloc{arena.get(), kArena};
  stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(size), &e, &alloc);
  if (!v) {
    if (err) *err = "cannot decode Ogg Vorbis (error " + std::to_string(e) + ")";
    return false;
  }
  stb_vorbis_info info = stb_vorbis_get_info(v);
  out.channels = info.channels;
  out.sampleRate = static_cast<int>(info.sample_rate);
  out.bitsPerSample = 32;
  if (out.channels <= 0 || out.sampleRate <= 0) {
    stb_vorbis_close(v);
    if (err) *err = "bad Ogg Vorbis header";
    return false;
  }
  unsigned total = stb_vorbis_stream_length_in_samples(v);
  if (total > 0 && total < (1u << 30)) out.interleaved.reserve(static_cast<size_t>(total) * static_cast<size_t>(out.channels));
  std::vector<float> chunk(static_cast<size_t>(4096 * out.channels));
  for (;;) {
    int n = stb_vorbis_get_samples_float_interleaved(v, out.channels, chunk.data(), static_cast<int>(chunk.size()));
    if (n <= 0) break;
    out.interleaved.insert(out.interleaved.end(), chunk.begin(), chunk.begin() + n * out.channels);
  }
  stb_vorbis_close(v);
  if (out.interleaved.empty()) {
    if (err) *err = "Ogg Vorbis stream has no audio";
    return false;
  }
  return true;
}

// Some FL loops interleave a second, non-audio logical stream (its own
// serial number) with the Vorbis pages. stb_vorbis reads every page it meets,
// so those packets would decode as noise: keep only the pages of the stream
// whose first packet is the Vorbis identification header. Returns false when
// the file has a single stream (nothing to filter).
bool keepVorbisStream(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
  struct Page {
    size_t at, len;
    uint32_t serial;
  };
  std::vector<Page> pages;
  bool vorbisFound = false, other = false;
  uint32_t vorbisSerial = 0;
  size_t p = 0;
  while (p + 27 <= n) {
    if (std::memcmp(d + p, "OggS", 4) != 0) {
      ++p;
      continue;
    }
    const size_t segs = d[p + 26];
    if (p + 27 + segs > n) break;
    size_t body = 0;
    for (size_t k = 0; k < segs; ++k) body += d[p + 27 + k];
    if (p + 27 + segs + body > n) break;
    uint32_t serial = d[p + 14] | (d[p + 15] << 8) | (d[p + 16] << 16) | (static_cast<uint32_t>(d[p + 17]) << 24);
    const uint8_t* first = d + p + 27 + segs;
    if (!vorbisFound && (d[p + 5] & 0x02) && body >= 7 && first[0] == 1 && std::memcmp(first + 1, "vorbis", 6) == 0) {
      vorbisFound = true;
      vorbisSerial = serial;
    }
    pages.push_back({p, 27 + segs + body, serial});
    p += 27 + segs + body;
  }
  if (!vorbisFound) return false;
  for (const auto& pg : pages) other = other || pg.serial != vorbisSerial;
  if (!other) return false;
  out.clear();
  out.reserve(n);
  for (const auto& pg : pages)
    if (pg.serial == vorbisSerial) out.insert(out.end(), d + pg.at, d + pg.at + pg.len);
  return true;
}

}  // namespace

bool decodeOggVorbis(const uint8_t* data, size_t size, DecodedAudio& out, std::string* err) {
  std::vector<uint8_t> vorbisOnly;
  if (keepVorbisStream(data, size, vorbisOnly)) return decodeOne(vorbisOnly.data(), vorbisOnly.size(), out, err);
  return decodeOne(data, size, out, err);
}

}  // namespace gl

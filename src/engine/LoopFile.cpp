// Gridline - loop facts of audio files. See LoopFile.h.
#include "LoopFile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <utility>
#include "Caf.h"
#include "Rex2.h"

namespace gl::loopfile {

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// 80-bit IEEE extended (AIFF sample rate).
double extended(const uint8_t* p) {
  const int exp = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mant = 0;
  for (int k = 0; k < 8; ++k) mant = (mant << 8) | p[2 + k];
  if (exp == 0 && mant == 0) return 0;
  const double v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

void addTag(Info& in, const std::string& t) {
  if (!t.empty() && std::find(in.tags.begin(), in.tags.end(), t) == in.tags.end()) in.tags.push_back(t);
}

// Quarter-note tempo of `beats` beats of 1/den notes lasting `seconds`.
double tempoOf(double beats, int den, double seconds) {
  if (!(beats > 0) || !(seconds > 0)) return 0;
  const double bpm = beats * 4.0 / (den > 0 ? den : 4) * 60.0 / seconds;
  return bpm >= 20 && bpm <= 999 ? bpm : 0;
}

bool fromRex(const uint8_t* p, size_t n, Info& out) {
  rex::Info r;
  if (!rex::parse(p, n, r, nullptr)) return false;
  out.format = Format::Sliced;
  out.sampleRate = r.sampleRate;
  out.frames = r.frames;
  uint32_t a = 0, b = 0;
  rex::loopRegion(r, a, b);
  out.loopStart = a, out.loopEnd = b;
  out.sigNum = r.sigNum > 0 ? r.sigNum : 4;
  out.sigDen = r.sigDen > 0 ? r.sigDen : 4;
  out.beats = rex::loopBeats(r);
  out.bpm = rex::tempo(r);
  out.loop = out.beats > 0;
  for (const rex::Slice& s : rex::slices(r)) out.slices.push_back(Slice{s.start, s.length, rex::beatOf(r, s.start), s.muted});
  return true;
}

bool fromCaf(const uint8_t* p, size_t n, Info& out) {
  caf::Info c;
  if (!caf::parse(p, n, c, nullptr)) return false;
  out.sampleRate = c.sampleRate;
  out.frames = c.frames();
  if (!c.hasLoop) return false;
  out.format = Format::Tagged;
  const caf::Loop l = caf::loopOf(c);
  out.sigNum = l.sigNum, out.sigDen = l.sigDen;
  out.key = l.key;
  out.minor = l.minor;
  out.hasMode = l.key >= 0 && !c.value("key type").empty();
  out.beats = l.beats;
  out.loop = l.beats > 0;
  // A tagged loop's tempo is its beats over its length (beats of the meter's note value).
  if (out.loop && out.sampleRate > 0 && out.frames > 0) out.bpm = tempoOf(l.beats, 4, static_cast<double>(out.frames) / out.sampleRate);
  out.loopEnd = out.frames;
  for (int64_t m : c.markers)
    if (m >= 0 && (out.frames <= 0 || m <= out.frames)) out.markers.push_back(m);
  std::sort(out.markers.begin(), out.markers.end());
  out.markers.erase(std::unique(out.markers.begin(), out.markers.end()), out.markers.end());
  // Beat markers sit on the loop's hits: they slice it (a slicer takes them as its markers).
  if (out.loop && out.frames > 0) {
    std::vector<int64_t> cuts;
    for (int64_t m : out.markers)
      if (m < out.frames && (cuts.empty() || m > cuts.back())) cuts.push_back(m);
    if (cuts.empty() || cuts.front() > 0) cuts.insert(cuts.begin(), 0);
    if (cuts.size() >= 2)
      for (size_t k = 0; k < cuts.size(); ++k) {
        const int64_t next = k + 1 < cuts.size() ? cuts[k + 1] : out.frames;
        out.slices.push_back(Slice{cuts[k], next - cuts[k], static_cast<double>(cuts[k]) / static_cast<double>(out.frames) * out.beats, false});
      }
  }
  for (const std::string& t : l.tags) addTag(out, t);
  return true;
}

// AIFF / AIFC: "COMM" (frames, rate) and the loop chunk "basc" (version,
// beats, root note, scale type, meter, loop type).
bool fromAiff(const uint8_t* p, size_t n, Info& out) {
  if (n < 12 || std::memcmp(p, "FORM", 4) != 0 || (std::memcmp(p + 8, "AIFF", 4) != 0 && std::memcmp(p + 8, "AIFC", 4) != 0)) return false;
  size_t at = 12;
  const size_t end = std::min<size_t>(n, 8 + static_cast<size_t>(be32(p + 4)));
  bool basc = false;
  for (int guard = 0; at + 8 <= end && guard < 512; ++guard) {
    const uint8_t* c = p + at;
    const uint32_t size = be32(c + 4);
    const size_t body = at + 8;
    if (size > end - body) break;
    const uint8_t* d = p + body;
    if (std::memcmp(c, "COMM", 4) == 0 && size >= 18) {
      out.frames = be32(d + 2);
      out.sampleRate = extended(d + 8);
    } else if (std::memcmp(c, "basc", 4) == 0 && size >= 18) {
      basc = true;
      const uint32_t beats = be32(d + 4);
      const uint16_t root = be16(d + 8), scale = be16(d + 10), num = be16(d + 12), den = be16(d + 14);
      if (beats > 0 && beats < 100000) out.beats = beats;
      if (num > 0 && num <= 64) out.sigNum = num;
      if (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32) out.sigDen = den;
      // Scale type 1 minor, 2 major (3 neither, 4 both): a key only with a mode.
      if (root > 0 && root < 128 && (scale == 1 || scale == 2)) {
        out.key = root % 12;
        out.minor = scale == 1;
        out.hasMode = true;
      }
    }
    at = body + size + (size & 1u);
  }
  if (!basc) return false;
  out.format = Format::Aiff;
  out.loop = out.beats > 0;
  if (out.loop && out.sampleRate > 0 && out.frames > 0) out.bpm = tempoOf(out.beats, out.sigDen, static_cast<double>(out.frames) / out.sampleRate);
  out.loopEnd = out.frames;
  return true;
}

// WAV: "acid" (flags, root note, beats, meter, tempo); "fmt " and "data" for the length.
bool fromWav(const uint8_t* p, size_t n, Info& out) {
  if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return false;
  size_t at = 12;
  bool acid = false;
  uint32_t blockAlign = 0, dataSize = 0, flags = 0;
  uint16_t root = 0;
  float tempo = 0;
  for (int guard = 0; at + 8 <= n && guard < 512; ++guard) {
    const uint8_t* c = p + at;
    const uint32_t size = le32(c + 4);
    const size_t body = at + 8;
    const uint8_t* d = p + body;
    const size_t have = n - body;
    if (std::memcmp(c, "fmt ", 4) == 0 && have >= 16 && size >= 16) {
      out.sampleRate = le32(d + 4);
      blockAlign = le16(d + 12);
    } else if (std::memcmp(c, "data", 4) == 0) {
      dataSize = size;
    } else if (std::memcmp(c, "acid", 4) == 0 && have >= 24 && size >= 24) {
      acid = true;
      flags = le32(d);
      root = le16(d + 4);
      const uint32_t beats = le32(d + 12);
      const uint16_t den = le16(d + 16), num = le16(d + 18);
      uint32_t bits = le32(d + 20);
      std::memcpy(&tempo, &bits, sizeof tempo);
      if (beats > 0 && beats < 100000) out.beats = beats;
      if (num > 0 && num <= 64) out.sigNum = num;
      if (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32) out.sigDen = den;
    }
    if (static_cast<uint64_t>(size) + (size & 1u) > have) break;
    at = body + size + (size & 1u);
  }
  if (!acid) return false;
  out.format = Format::Wav;
  if (blockAlign > 0) out.frames = dataSize / blockAlign;
  // Flag bit 0: one-shot; bit 1: the root note is set.
  out.loop = (flags & 1u) == 0 && std::isfinite(tempo) && tempo >= 20 && tempo <= 999;
  if (out.loop) out.bpm = tempo;
  if ((flags & 2u) && root < 128) out.key = root % 12;
  out.loopEnd = out.frames;
  return true;
}

}  // namespace

const char* formatTag(Format f) {
  switch (f) {
    case Format::Sliced: return "rex";
    case Format::Tagged: return "apple loop";
    case Format::Aiff:
    case Format::Wav:
    case Format::None: break;
  }
  return "";
}

bool readBytes(const uint8_t* p, size_t n, Info& out) {
  out = Info{};
  bool ok = false;
  if (rex::looksLikeRex2(p, n)) ok = fromRex(p, n, out);
  else if (caf::looksLikeCaf(p, n)) ok = fromCaf(p, n, out);
  else if (n >= 4 && std::memcmp(p, "FORM", 4) == 0) ok = fromAiff(p, n, out);
  else if (n >= 4 && std::memcmp(p, "RIFF", 4) == 0) ok = fromWav(p, n, out);
  if (!ok) {
    out = Info{};
    return false;
  }
  addTag(out, formatTag(out.format));
  addTag(out, out.loop ? "loop" : "one-shot");
  if (!out.loop) out.bpm = 0;
  return true;
}

bool read(const std::string& path, Info& out) {
  out = Info{};
  std::error_code ec;
  const std::filesystem::path fp = std::filesystem::u8path(path);
  const uintmax_t size = std::filesystem::file_size(fp, ec);
  if (ec || size < 12) return false;
  std::ifstream is(fp, std::ios::binary);
  uint8_t head[12] = {};
  if (!is || !is.read(reinterpret_cast<char*>(head), 12)) return false;
  // Only the formats that carry loop facts are read further.
  const bool wanted = std::memcmp(head, "CAT ", 4) == 0 || std::memcmp(head, "caff", 4) == 0 || std::memcmp(head, "FORM", 4) == 0 ||
                      std::memcmp(head, "RIFF", 4) == 0;
  if (!wanted) return false;
  // Loops are small: the whole file. A long recording: its first megabyte, where the loop chunks of
  // the files that have them sit.
  const size_t want = size <= (64u << 20) ? static_cast<size_t>(size) : (1u << 20);
  std::vector<uint8_t> bytes(want);
  std::memcpy(bytes.data(), head, 12);
  is.read(reinterpret_cast<char*>(bytes.data() + 12), static_cast<std::streamsize>(want - 12));
  bytes.resize(12 + static_cast<size_t>(is.gcount()));
  return readBytes(bytes.data(), bytes.size(), out);
}

int64_t loopFrames(const Info& li, int64_t frames) {
  if (frames <= 0) return 0;
  const int64_t a = std::max<int64_t>(0, li.loopStart), b = li.loopEnd > 0 ? std::min(li.loopEnd, frames) : frames;
  return b > a ? b - a : frames;
}

int64_t loopFramesOf(const std::string& path, int64_t frames) {
  static std::mutex m;
  static std::map<std::string, std::pair<int64_t, int64_t>> regions;  // path: loop start, end (0, 0: none)
  std::pair<int64_t, int64_t> r;
  {
    std::lock_guard<std::mutex> lock(m);
    auto it = regions.find(path);
    if (it != regions.end()) r = it->second;
    else {
      Info li;
      if (read(path, li)) r = {li.loopStart, li.loopEnd};
      regions[path] = r;
    }
  }
  Info li;
  li.loopStart = r.first, li.loopEnd = r.second;
  return loopFrames(li, frames);
}

}  // namespace gl::loopfile

// Gridline — loop files (K-161): sliced loops (.rx2: container, slices,
// tempo, the compressed audio decoded sample for sample), tagged loops (.caf:
// linear PCM, AAC through the computer's decoder, loop metadata and beat
// markers), AIFF and WAV loop chunks, the browser's loop facts and the slicer
// channel a sliced loop becomes. Every file is written by this test; no third-
// party loop is used. With GRIDLINE_LOOP_ORACLE_DIR set to a folder of
// public-domain reference loops (the VelociLoops test files) it also checks
// the decoder against their published sample values.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "engine/Caf.h"
#include "engine/KerfMarks.h"
#include "engine/LoopFile.h"
#include "engine/LoopImport.h"
#include "engine/Rex2.h"
#include "engine/Sample.h"
#include "engine/SlicexEngine.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;
constexpr double kPi = 3.14159265358979323846;

void put32(Bytes& b, uint32_t v) {
  for (int k = 3; k >= 0; --k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}
void put64(Bytes& b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32));
  put32(b, static_cast<uint32_t>(v));
}
void le32(Bytes& b, uint32_t v) {
  for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
void le16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
// An IFF chunk (big-endian size, padded to even).
void chunk(Bytes& b, const char* id, const Bytes& body) {
  tag(b, id);
  put32(b, static_cast<uint32_t>(body.size()));
  b.insert(b.end(), body.begin(), body.end());
  if (body.size() & 1) b.push_back(0);
}
void cat(Bytes& b, const char* type, const Bytes& inner) {
  Bytes body;
  tag(body, type);
  body.insert(body.end(), inner.begin(), inner.end());
  chunk(b, "CAT ", body);
}
void writeFile(const std::string& path, const Bytes& b) {
  std::ofstream os(path, std::ios::binary);
  os.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
Bytes readFile(const std::string& path) {
  std::ifstream is(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

// ------------------------------------------------- test-only REX2 encoder
// Written from the same format description as the decoder: the decoder's state
// machine run backwards (choose the order with the smallest magnitude, find
// the residual that rebuilds the sample, code it as a zero run, a one bit and
// a truncated binary remainder).
struct BitWriter {
  Bytes out;
  uint32_t word = 0;
  int n = 0;
  void bit(uint32_t b) {
    word = (word << 1) | (b & 1u);
    if (++n == 32) {
      put32(out, word);
      word = 0, n = 0;
    }
  }
  void bits(uint32_t v, int count) {
    for (int k = count - 1; k >= 0; --k) bit((v >> k) & 1u);
  }
  void flush() {
    if (n > 0) {
      put32(out, word << (32 - n));
      word = 0, n = 0;
    }
  }
};

struct EncChannel {
  int32_t d[5] = {0, 0, 0, 0, 0};
  uint32_t avg[5] = {2560, 2560, 2560, 2560, 2560};
  uint32_t j = 2;
  int rbits = 0;
};

bool encodeOne(EncChannel& ch, BitWriter& bw, int32_t s) {
  int idx = 0;
  for (int k = 1; k < 5; ++k)
    if (ch.avg[k] < ch.avg[idx]) idx = k;
  int32_t* d = ch.d;
  int32_t r = s;
  for (int k = 0; k < idx; ++k) r -= d[k];
  const uint32_t code = r >= 0 ? static_cast<uint32_t>(r) : static_cast<uint32_t>(-static_cast<int64_t>(r) - 1);
  uint64_t step = ((static_cast<uint64_t>(ch.avg[idx]) * 3u) + 36u) >> 7;
  uint64_t prefix = 0;
  int window = 7, zeros = 0;
  for (;;) {
    if (step > 0 ? code < prefix + step : code == prefix) break;
    prefix += step;
    ++zeros;
    if (--window == 0) step <<= 2, window = 7;
    if (zeros > 150) return false;
  }
  for (int k = 0; k < zeros; ++k) bw.bit(0);
  bw.bit(1);
  if (step < ch.j) {
    for (uint32_t jt = ch.j >> 1; step < jt; jt >>= 1) ch.j = jt, --ch.rbits;
  } else {
    while (step >= ch.j) ch.j <<= 1, ++ch.rbits;
  }
  const uint64_t rem = code - prefix, threshold = ch.j - step;
  if (rem < threshold) {
    if (ch.rbits > 0) bw.bits(static_cast<uint32_t>(rem), ch.rbits);
  } else {
    const uint64_t v = rem + threshold;
    if (ch.rbits > 0) bw.bits(static_cast<uint32_t>(v >> 1), ch.rbits);
    bw.bit(static_cast<uint32_t>(v & 1u));
  }
  // The decoder's state update for the same residual.
  switch (idx) {
    case 0: {
      const int32_t t0 = r - d[0], t1 = t0 - d[1], t2 = t1 - d[2];
      d[4] = t2 - d[3], d[3] = t2, d[2] = t1, d[1] = t0, d[0] = r;
      break;
    }
    case 1: {
      const int32_t t1 = r - d[1], t2 = t1 - d[2], sm = d[0] + r;
      d[4] = t2 - d[3], d[3] = t2, d[2] = t1, d[1] = r, d[0] = sm;
      break;
    }
    case 2: {
      const int32_t n1 = d[1] + r, sm = d[0] + n1, t = r - d[2];
      d[4] = t - d[3], d[3] = t, d[2] = r, d[1] = n1, d[0] = sm;
      break;
    }
    case 3: {
      const int32_t n2 = d[2] + r, n1 = d[1] + n2, sm = d[0] + n1;
      d[4] = r - d[3], d[3] = r, d[2] = n2, d[1] = n1, d[0] = sm;
      break;
    }
    default: {
      const int32_t n3 = d[3] + r, n2 = d[2] + n3, n1 = d[1] + n2, sm = d[0] + n1;
      d[4] = r, d[3] = n3, d[2] = n2, d[1] = n1, d[0] = sm;
      break;
    }
  }
  for (int k = 0; k < 5; ++k) ch.avg[k] = ch.avg[k] + static_cast<uint32_t>(d[k] ^ (d[k] >> 31)) - (ch.avg[k] >> 5);
  return true;
}

// Interleaved integer PCM -> compressed audio.
Bytes encodeAudio(const std::vector<int32_t>& pcm, int channels) {
  BitWriter bw;
  EncChannel c0, c1;
  const size_t frames = pcm.size() / static_cast<size_t>(channels);
  for (size_t f = 0; f < frames; ++f) {
    const int32_t l2 = pcm[f * static_cast<size_t>(channels)] * 2;
    encodeOne(c0, bw, l2);
    if (channels == 2) encodeOne(c1, bw, pcm[f * 2 + 1] * 2 - l2);
  }
  bw.flush();
  return bw.out;
}

struct RexSpec {
  int channels = 1, bits = 16;
  uint32_t rate = 44100;
  std::vector<int32_t> pcm;  // interleaved
  uint32_t loopStart = 0, loopEnd = 0;
  uint16_t bars = 1;
  uint8_t beats = 0, num = 4, den = 4;
  uint32_t tempoMilli = 120000;
  struct E {
    uint32_t start, length;
    uint16_t points;
    uint8_t flags;
  };
  std::vector<E> entries;
  bool shuffle = false;   // chunks in another order, an unknown chunk, an odd-sized one
  bool noSlices = false;
};

Bytes makeRex(const RexSpec& s) {
  const uint32_t frames = static_cast<uint32_t>(s.pcm.size() / static_cast<size_t>(s.channels));
  Bytes head;
  put32(head, 0x490cf18du);
  head.push_back(0xbc), head.push_back(0x02);
  head.resize(29, 0);
  Bytes glob;
  put32(glob, static_cast<uint32_t>(s.entries.size()));
  put16(glob, s.bars);
  glob.push_back(s.beats), glob.push_back(s.num), glob.push_back(s.den), glob.push_back(78);
  put16(glob, 0), put16(glob, 1000), put16(glob, 0);
  put32(glob, s.tempoMilli);
  glob.push_back(1), glob.push_back(0);
  Bytes slcl;
  for (const auto& e : s.entries) {
    Bytes b;
    put32(b, e.start), put32(b, e.length), put16(b, e.points);
    b.push_back(e.flags);
    chunk(slcl, "SLCE", b);
  }
  Bytes sinf;
  sinf.push_back(static_cast<uint8_t>(s.channels));
  sinf.push_back(s.bits == 24 ? 5 : 3);
  put32(sinf, s.rate), put32(sinf, frames), put32(sinf, s.loopStart), put32(sinf, s.loopEnd ? s.loopEnd : frames);
  const Bytes sdat = encodeAudio(s.pcm, s.channels);
  Bytes inner;
  if (!s.shuffle) {
    chunk(inner, "HEAD", head);
    chunk(inner, "GLOB", glob);
    if (!s.noSlices) cat(inner, "SLCL", slcl);
    chunk(inner, "SINF", sinf);
    chunk(inner, "SDAT", sdat);
  } else {
    chunk(inner, "SDAT", sdat);
    chunk(inner, "ZZZZ", Bytes{1, 2, 3});  // unknown and odd-sized: skipped, padded
    chunk(inner, "SINF", sinf);
    Bytes deep;
    cat(deep, "SLCL", slcl);
    cat(inner, "DEVL", deep);  // the slice list one level deeper
    chunk(inner, "GLOB", glob);
    chunk(inner, "HEAD", head);
  }
  Bytes file;
  cat(file, "REX2", inner);
  return file;
}

uint32_t gSeed = 12345;
int32_t noise(int bits) {
  gSeed = gSeed * 1664525u + 1013904223u;
  const int32_t range = 1 << (bits - 1);
  return static_cast<int32_t>(gSeed >> 8) % range - range / 2;
}

// Test material: a decaying tone with hits, silence, a full-scale burst and noise.
std::vector<int32_t> material(size_t frames, int channels, int bits) {
  std::vector<int32_t> v(frames * static_cast<size_t>(channels));
  const double full = std::ldexp(1.0, bits - 1) - 1;
  for (size_t f = 0; f < frames; ++f)
    for (int c = 0; c < channels; ++c) {
      const double t = static_cast<double>(f % 5000) / 44100.0;
      double x = std::sin(2 * kPi * (110.0 + 55 * c) * t) * std::exp(-t * 9.0) * 0.8;
      if (f % 5000 < 3) x = c ? -0.99 : 0.99;  // hits
      int32_t s = static_cast<int32_t>(std::lround(x * full));
      if (f >= frames / 2 && f < frames / 2 + 400) s = 0;                                  // silence
      if (f >= frames / 2 + 400 && f < frames / 2 + 600) s = (f & 8) ? static_cast<int32_t>(full) : -static_cast<int32_t>(full) - 1;  // square at full scale
      if (f >= frames - 2000) s = noise(bits);                                            // noise
      v[f * static_cast<size_t>(channels) + static_cast<size_t>(c)] = s;
    }
  return v;
}

// -------------------------------------------------------------- REX2 tests

void testDwopKnownBits() {
  // Hand-built streams from the format rules alone (not the encoder): the first
  // sample's step is (2560 * 3 + 36) >> 7 = 60, so 5 remainder bits (threshold 4).
  auto decode1 = [](std::vector<uint8_t> bytes, uint32_t frames, std::vector<int32_t>& out) {
    return rex::decodePcm(bytes.data(), bytes.size(), 1, 16, frames, out, nullptr);
  };
  auto pack = [](const std::string& bits) {
    BitWriter bw;
    for (char c : bits) bw.bit(c == '1');
    for (int k = 0; k < 64; ++k) bw.bit(0);
    bw.flush();
    return bw.out;
  };
  std::vector<int32_t> o;
  // "1" + "00010": code 2 = residual +2 -> sample 1; then "1 00000": code 0 -> 0.
  expect(decode1(pack("100010" "100000"), 2, o) && o.size() == 2 && o[0] == 1 && o[1] == 0, "dwop bits: a small positive sample, then zero");
  // Code 1 (odd) is the residual -2: sample -1.
  expect(decode1(pack("100001"), 1, o) && o[0] == -1, "dwop bits: odd codes are negative");
  // Remainder 7 >= threshold 4 takes one more bit: 7 * 2 - 4 + 0 = 10 -> sample 5.
  expect(decode1(pack("1001110"), 1, o) && o[0] == 5, "dwop bits: the extended remainder");
  // One zero adds the step: prefix 60 + remainder 3 = 63 (odd) -> residual -64 -> sample -32.
  expect(decode1(pack("0100011"), 1, o) && o[0] == -32, "dwop bits: a zero run adds the step");
  // All zero bits never end a code: damaged data, refused instead of looping.
  std::vector<uint8_t> zeros(64, 0);
  expect(!rex::decodePcm(zeros.data(), zeros.size(), 1, 16, 4, o, nullptr), "dwop bits: an endless zero run is refused");
}

void testDwopRoundTrip() {
  for (int channels : {1, 2})
    for (int bits : {16, 24}) {
      gSeed = 777u + static_cast<uint32_t>(channels * 10 + bits);
      const std::vector<int32_t> pcm = material(30000, channels, bits);
      const Bytes data = encodeAudio(pcm, channels);
      std::vector<int32_t> out;
      const bool ok = rex::decodePcm(data.data(), data.size(), channels, bits, 30000, out, nullptr);
      char name[96];
      std::snprintf(name, sizeof name, "dwop round trip bit-exact: %s %d-bit", channels == 1 ? "mono" : "stereo", bits);
      expect(ok && out == pcm, name);
    }
  // Truncated data: refused cleanly, nothing past the buffer read.
  const std::vector<int32_t> pcm = material(8000, 1, 16);
  Bytes data = encodeAudio(pcm, 1);
  data.resize(data.size() / 3);
  std::vector<int32_t> out;
  expect(!rex::decodePcm(data.data(), data.size(), 1, 16, 8000, out, nullptr), "dwop: audio that ends early fails");
}

RexSpec basicSpec() {
  RexSpec s;
  s.channels = 1;
  gSeed = 99;
  s.pcm = material(44100 * 2 + 300, 1, 16);
  s.loopStart = 300;
  s.loopEnd = 300 + 44100 * 2;  // 4 beats at 120 BPM
  s.bars = 1;
  s.tempoMilli = 140000;  // the preview tempo is not the loop's own
  // Two slices before the loop start's region? No: one marker before the loop, slices in it,
  // analysis markers between, a muted slice and a gated one.
  s.entries = {{100, 200, 0x7fff, 0},          // before the loop region: not played
               {300, 22050, 0x7fff, 0},        // beat 0
               {11000, 1, 0x1200, 0},          // analysis marker: not a slice
               {22350, 22050, 0x7fff, 1},      // beat 1, muted
               {44400, 10000, 0x7fff, 0},      // beat 2, gated (stops early)
               {66450, 22050, 0x7fff, 0}};     // beat 3
  return s;
}

void testRexContainer() {
  RexSpec s = basicSpec();
  const Bytes file = makeRex(s);
  rex::Info in;
  std::string err;
  expect(rex::parse(file.data(), file.size(), in, &err), "rex: parses");
  expect(in.channels == 1 && in.bits == 16 && in.sampleRate == 44100 && in.frames == s.pcm.size(), "rex: audio format");
  expect(in.entries.size() == 6 && in.loopStart == 300 && in.loopEnd == 300 + 88200, "rex: slice entries and loop region");
  expect(std::fabs(rex::loopBeats(in) - 4) < 1e-9, "rex: four beats from one bar of 4/4");
  expect(std::fabs(rex::tempo(in) - 120) < 1e-6, "rex: own tempo from the loop's beats and length (not the preview tempo)");
  expect(std::fabs(rex::previewTempo(in) - 140) < 1e-9, "rex: preview tempo kept apart");
  const auto sl = rex::slices(in);
  expect(sl.size() == 4, "rex: slices inside the loop, markers left out");
  expect(sl.size() == 4 && sl[0].start == 300 && sl[1].start == 22350 && sl[1].muted && sl[2].length == 10000 && sl[3].length == 22050,
         "rex: starts, muted flag, gated length");
  expect(sl.size() == 4 && std::fabs(rex::beatOf(in, sl[2].start) - 2.0) < 0.01, "rex: beat of a slice");
  DecodedAudio d;
  expect(rex::decode(file.data(), file.size(), d, nullptr, &err) && d.frames() == s.pcm.size() && d.channels == 1, "rex: decodes");
  bool same = d.frames() == s.pcm.size();
  for (size_t k = 0; same && k < s.pcm.size(); ++k) same = d.interleaved[k] == static_cast<float>(s.pcm[k]) / 32768.0f;
  expect(same, "rex: decoded floats are the samples / 32768");

  RexSpec sh = basicSpec();
  sh.shuffle = true;
  const Bytes f2 = makeRex(sh);
  rex::Info in2;
  expect(rex::parse(f2.data(), f2.size(), in2, &err) && in2.entries.size() == 6 && in2.frames == in.frames, "rex: chunk order, nesting, unknown and odd chunks");
  DecodedAudio d2;
  expect(rex::decode(f2.data(), f2.size(), d2, nullptr, nullptr) && d2.interleaved == d.interleaved, "rex: same audio in any chunk order");

  RexSpec ns = basicSpec();
  ns.noSlices = true;
  const Bytes f3 = makeRex(ns);
  rex::Info in3;
  expect(rex::parse(f3.data(), f3.size(), in3, nullptr) && rex::slices(in3).size() == 1 && rex::slices(in3)[0].start == 300,
         "rex: no slice list: one slice from the loop start");

  // Stereo 24-bit in 7/8 (beats are eighth notes: the tempo counts quarters).
  RexSpec st;
  st.channels = 2, st.bits = 24;
  gSeed = 5;
  st.pcm = material(44100 * 2, 2, 24);
  st.bars = 1, st.num = 7, st.den = 8;
  st.entries = {{0, 30000, 0x7fff, 0}, {30000, 58200, 0x7fff, 0}};
  const Bytes f4 = makeRex(st);
  rex::Info in4;
  DecodedAudio d4;
  expect(rex::parse(f4.data(), f4.size(), in4, nullptr) && std::fabs(rex::loopBeats(in4) - 7) < 1e-9, "rex: 7/8 is seven beats");
  expect(std::fabs(rex::tempo(in4) - 105) < 1e-6, "rex: 7/8 over two seconds is 105 quarter-note BPM");
  expect(rex::decode(f4.data(), f4.size(), d4, nullptr, nullptr) && d4.channels == 2 && d4.bitsPerSample == 24 &&
             d4.interleaved[1] == static_cast<float>(st.pcm[1]) / 8388608.0f,
         "rex: stereo 24-bit decodes");
}

void testRexDamage() {
  const Bytes good = makeRex(basicSpec());
  std::string err;
  Bytes b = good;
  b[8] = 'X';
  rex::Info in;
  expect(!rex::parse(b.data(), b.size(), in, &err), "rex damage: not a sliced loop");
  b = good;
  b.resize(40);
  expect(!rex::parse(b.data(), b.size(), in, &err), "rex damage: cut short");
  // A chunk size past its container.
  b = good;
  b[12 + 4] = 0x7f;
  expect(!rex::parse(b.data(), b.size(), in, &err), "rex damage: a chunk larger than the file");
  // Random damage: never a crash, the output (when any) finite and within range.
  uint32_t seed = 4242;
  int decoded = 0;
  bool finite = true;
  for (int it = 0; it < 2000; ++it) {
    Bytes m = good;
    const int edits = 1 + static_cast<int>(seed % 6);
    for (int e = 0; e < edits; ++e) {
      seed = seed * 1664525u + 1013904223u;
      const size_t at = (seed >> 4) % m.size();
      seed = seed * 1664525u + 1013904223u;
      m[at] = static_cast<uint8_t>(seed >> 24);
    }
    if (it % 7 == 0) m.resize((seed >> 3) % m.size());
    DecodedAudio d;
    if (rex::decode(m.data(), m.size(), d, nullptr, nullptr)) {
      ++decoded;
      for (float x : d.interleaved) finite = finite && std::isfinite(x) && x >= -1.0f && x <= 1.0f;
    }
    loopfile::Info li;
    loopfile::readBytes(m.data(), m.size(), li);
  }
  expect(finite, "rex damage: 2000 damaged files, no crash, output finite and in range");
  std::printf("# %d of 2000 damaged files still decoded\n", decoded);
}

// ------------------------------------------------------------- CAF writer

struct CafSpec {
  std::string format = "lpcm";
  uint32_t flags = 0;  // 1 float, 2 little-endian
  uint32_t bits = 16, channels = 1;
  double rate = 44100;
  Bytes audio;  // already encoded
  std::vector<std::pair<std::string, std::string>> meta;
  std::vector<uint32_t> markers;
  bool sizeToEnd = false;  // data chunk size -1
};

Bytes makeCaf(const CafSpec& s) {
  Bytes b;
  tag(b, "caff");
  put16(b, 1), put16(b, 0);
  auto ch = [&b](const char* id, const Bytes& body, bool toEnd = false) {
    tag(b, id);
    put64(b, toEnd ? ~uint64_t{0} : body.size());
    b.insert(b.end(), body.begin(), body.end());
  };
  Bytes desc;
  uint64_t rb;
  std::memcpy(&rb, &s.rate, 8);
  put64(desc, rb);
  desc.insert(desc.end(), s.format.begin(), s.format.end());
  put32(desc, s.flags);
  put32(desc, (s.bits / 8) * s.channels), put32(desc, 1), put32(desc, s.channels), put32(desc, s.bits);
  ch("desc", desc);
  if (!s.meta.empty()) {
    Bytes u = {0x29, 0x81, 0x92, 0x73, 0xb5, 0xbf, 0x4a, 0xef, 0xb7, 0x8d, 0x62, 0xd1, 0xef, 0x90, 0xbb, 0x2c};
    put32(u, static_cast<uint32_t>(s.meta.size()));
    for (const auto& [k, v] : s.meta) {
      u.insert(u.end(), k.begin(), k.end()), u.push_back(0);
      u.insert(u.end(), v.begin(), v.end()), u.push_back(0);
    }
    ch("uuid", u);
  }
  if (!s.markers.empty()) {
    Bytes u = {0x03, 0x52, 0x81, 0x1b, 0x9d, 0x5d, 0x42, 0xe1, 0x88, 0x2d, 0x6a, 0xf6, 0x1a, 0x6b, 0x33, 0x0c};
    put32(u, 0), put32(u, 0x00010000), put16(u, 0x32), put16(u, 0x10), put32(u, 0), put32(u, static_cast<uint32_t>(s.markers.size()));
    for (uint32_t m : s.markers) put16(u, 1), put16(u, 0), put32(u, 0), put32(u, m);
    ch("uuid", u);
  }
  Bytes data;
  put32(data, 0);  // edit count
  data.insert(data.end(), s.audio.begin(), s.audio.end());
  ch("data", data, s.sizeToEnd);
  return b;
}

void testCaf() {
  const std::string dir = gl::test::hermeticTestEnv("loopfiles") + "/caf";
  fs::create_directories(dir);
  const size_t frames = 44100;  // one second
  std::vector<float> ref(frames * 2);
  for (size_t f = 0; f < frames; ++f) {
    ref[f * 2] = static_cast<float>(0.5 * std::sin(2 * kPi * 220.0 * static_cast<double>(f) / 44100.0));
    ref[f * 2 + 1] = -ref[f * 2] * 0.5f;
  }
  // int16 big-endian, float32 little-endian, int24 big-endian.
  struct Fmt {
    const char* name;
    uint32_t flags, bits;
  };
  for (const Fmt& fm : {Fmt{"int16 big-endian", 0, 16}, Fmt{"float32 little-endian", 3, 32}, Fmt{"int24 big-endian", 0, 24}}) {
    CafSpec s;
    s.flags = fm.flags, s.bits = fm.bits, s.channels = 2;
    for (float x : ref) {
      if (fm.flags & 1) {
        uint32_t u;
        std::memcpy(&u, &x, 4);
        le32(s.audio, u);
      } else {
        const int32_t v = static_cast<int32_t>(std::lround(x * (fm.bits == 16 ? 32767.0 : 8388607.0)));
        if (fm.bits == 16) put16(s.audio, static_cast<uint16_t>(v));
        else s.audio.push_back(static_cast<uint8_t>(v >> 16)), s.audio.push_back(static_cast<uint8_t>(v >> 8)), s.audio.push_back(static_cast<uint8_t>(v));
      }
    }
    s.sizeToEnd = fm.bits == 24;
    const std::string path = dir + "/pcm" + std::to_string(fm.bits) + ".caf";
    writeFile(path, makeCaf(s));
    SamplePtr smp = loadSample(path);
    double err = 0;
    if (smp && smp->frames() == frames)
      for (size_t f = 0; f < frames; ++f) err = std::max({err, static_cast<double>(std::fabs(smp->left[f] - ref[f * 2])), static_cast<double>(std::fabs(smp->right[f] - ref[f * 2 + 1]))});
    char name[96];
    std::snprintf(name, sizeof name, "caf: %s linear PCM decodes", fm.name);
    expect(smp && smp->frames() == frames && err < (fm.bits == 16 ? 1e-4 : 1e-6), name);
  }
  // Loop metadata and beat markers: 2 beats over one second = 120 BPM.
  CafSpec s;
  for (size_t f = 0; f < frames; ++f) put16(s.audio, static_cast<uint16_t>(static_cast<int16_t>(std::lround(ref[f * 2] * 32767))));
  s.meta = {{"category", "Drums"}, {"subcategory", "Drum Kit"}, {"genre", "Electronic/Dance"}, {"beat count", "2"},
            {"time signature", "4/4"}, {"key signature", "F#"}, {"key type", "minor"}, {"descriptors", "Grooving, Single,Part"}};
  s.markers = {0, 11025, 22050, 33075, 44100};
  const std::string loop = dir + "/loop.caf";
  writeFile(loop, makeCaf(s));
  caf::Info ci;
  const Bytes lb = readFile(loop);
  expect(caf::parse(lb.data(), lb.size(), ci, nullptr) && ci.hasLoop && ci.markers.size() == 5 && ci.frames() == static_cast<int64_t>(frames),
         "caf: loop chunk, markers, length");
  const caf::Loop l = caf::loopOf(ci);
  expect(l.beats == 2 && l.sigNum == 4 && l.key == 6 && l.minor, "caf: beats, meter, key F# minor");
  const std::vector<std::string> want = {"drums", "drum kit", "electronic/dance", "grooving", "single", "part"};
  expect(l.tags == want, "caf: category, subcategory, genre and descriptors as tags");
  loopfile::Info li;
  expect(loopfile::read(loop, li) && li.format == loopfile::Format::Tagged && li.loop && std::fabs(li.bpm - 120) < 1e-6,
         "loop facts: a tagged loop's tempo from its beats and length");
  expect(li.slices.size() == 4 && li.slices[2].start == 22050 && std::fabs(li.slices[2].beat - 1.0) < 1e-9, "loop facts: beat markers cut it");
  expect(std::find(li.tags.begin(), li.tags.end(), "apple loop") != li.tags.end() && std::find(li.tags.begin(), li.tags.end(), "loop") != li.tags.end(),
         "loop facts: format and loop tags");
  // A one-shot (no beat count): no tempo, whatever its length.
  CafSpec one = s;
  one.meta = {{"category", "Drums"}, {"key signature", "C"}};
  one.markers.clear();
  const std::string shot = dir + "/oneshot 128bpm.caf";
  writeFile(shot, makeCaf(one));
  expect(loopfile::read(shot, li) && !li.loop && li.bpm == 0 && std::find(li.tags.begin(), li.tags.end(), "one-shot") != li.tags.end(),
         "loop facts: a one-shot has no tempo");
  browse::FileMeta m = browse::Index::describe(shot, true);
  expect(m.bpm == 0 && m.key == 0, "browser: a tagged one-shot ignores the tempo in its name");
  m = browse::Index::describe(loop, true);
  expect(std::fabs(m.bpm - 120) < 1e-6 && m.key == 6 && m.minor && m.slices == 0, "browser: tagged loop tempo and key");
  expect(browse::matches(m, browse::parseQuery("tag:apple bpm:118-122 key:F#m")) && browse::matches(m, browse::parseQuery("grooving")),
         "browser: search by the file's own tags, tempo and key");
  expect(isAudioFile(loop) && browse::kindOf(loop) == browse::Kind::Audio, "caf is audio");

  // Damaged CAF files fail cleanly.
  Bytes bad = lb;
  bad.resize(30);
  expect(!caf::parse(bad.data(), bad.size(), ci, nullptr), "caf damage: no audio description");
  uint32_t seed = 99;
  bool ok = true;
  for (int it = 0; it < 500; ++it) {
    Bytes mm = lb;
    for (int e = 0; e < 4; ++e) {
      seed = seed * 1664525u + 1013904223u;
      mm[(seed >> 5) % mm.size()] = static_cast<uint8_t>(seed >> 24);
    }
    caf::Info x;
    if (caf::parse(mm.data(), mm.size(), x, nullptr) && x.lpcm()) {
      DecodedAudio d;
      if (caf::decode(dir + "/x.caf", mm.data(), mm.size(), d, nullptr))
        for (float v : d.interleaved) ok = ok && std::isfinite(v);
    }
    loopfile::Info y;
    loopfile::readBytes(mm.data(), mm.size(), y);
  }
  expect(ok, "caf damage: 500 damaged files, no crash");
}

// A CAF file around a raw AAC (ADTS) stream: the packets, their configuration in the cookie and a
// packet table stating the priming and the real length (how FFmpeg-encoded AAC is put into CAF here,
// as FFmpeg cannot write AAC in CAF itself).
[[maybe_unused]] Bytes cafFromAdts(const Bytes& adts, uint32_t frames, uint32_t priming) {
  std::vector<std::pair<size_t, size_t>> packets;  // payload offset, size
  int profile = 1, freq = 4, chans = 1;
  for (size_t at = 0; at + 7 <= adts.size();) {
    if (adts[at] != 0xFF || (adts[at + 1] & 0xF0) != 0xF0) break;
    const bool crc = !(adts[at + 1] & 1);
    const size_t len = ((adts[at + 3] & 3u) << 11) | (static_cast<size_t>(adts[at + 4]) << 3) | (adts[at + 5] >> 5);
    profile = adts[at + 2] >> 6, freq = (adts[at + 2] >> 2) & 15, chans = ((adts[at + 2] & 1) << 2) | (adts[at + 3] >> 6);
    const size_t head = crc ? 9 : 7;
    if (len <= head || at + len > adts.size()) break;
    packets.push_back({at + head, len - head});
    at += len;
  }
  static const double rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
  Bytes b;
  tag(b, "caff");
  put16(b, 1), put16(b, 0);
  auto ch = [&b](const char* id, const Bytes& body) {
    tag(b, id);
    put64(b, body.size());
    b.insert(b.end(), body.begin(), body.end());
  };
  Bytes desc;
  uint64_t rb;
  std::memcpy(&rb, &rates[freq], 8);
  put64(desc, rb);
  tag(desc, "aac ");
  put32(desc, 2), put32(desc, 0), put32(desc, 1024), put32(desc, static_cast<uint32_t>(chans)), put32(desc, 0);
  ch("desc", desc);
  const uint8_t asc0 = static_cast<uint8_t>(((profile + 1) << 3) | (freq >> 1)), asc1 = static_cast<uint8_t>(((freq & 1) << 7) | (chans << 3));
  Bytes kuki = {0x03, 0x19, 0x00, 0x00, 0x00, 0x04, 0x11, 0x40, 0x15, 0x00, 0x18, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x01, 0xF4, 0x00,
                0x05, 0x02, asc0, asc1, 0x06, 0x01, 0x02};
  ch("kuki", kuki);
  Bytes pakt;
  const uint64_t total = packets.size() * 1024;
  put64(pakt, packets.size()), put64(pakt, frames), put32(pakt, priming), put32(pakt, static_cast<uint32_t>(total - frames - priming));
  for (const auto& p : packets) {
    size_t v = p.second;
    uint8_t tmp[5];
    int n = 0;
    do tmp[n++] = static_cast<uint8_t>(v & 0x7F), v >>= 7;
    while (v);
    for (int k = n - 1; k >= 0; --k) pakt.push_back(static_cast<uint8_t>(tmp[k] | (k ? 0x80 : 0)));
  }
  ch("pakt", pakt);
  Bytes data;
  put32(data, 0);
  for (const auto& p : packets) data.insert(data.end(), adts.begin() + static_cast<std::ptrdiff_t>(p.first), adts.begin() + static_cast<std::ptrdiff_t>(p.first + p.second));
  ch("data", data);
  return b;
}

// AAC in CAF through the computer's decoder (made from our own WAV by the system's converter).
void testCafAac() {
  const std::string dir = gl::test::hermeticTestEnv("loopfiles") + "/aac";
  fs::create_directories(dir);
  const std::string wav = dir + "/tone.wav", caf = dir + "/tone.caf";
  const uint32_t frames = 44100;
  Bytes w;
  tag(w, "RIFF"), le32(w, 36 + frames * 2), tag(w, "WAVE"), tag(w, "fmt "), le32(w, 16), le16(w, 1), le16(w, 1), le32(w, 44100), le32(w, 88200), le16(w, 2),
      le16(w, 16), tag(w, "data"), le32(w, frames * 2);
  for (uint32_t f = 0; f < frames; ++f) le16(w, static_cast<uint16_t>(static_cast<int16_t>(std::lround(12000 * std::sin(2 * kPi * 440.0 * f / 44100.0)))));
  writeFile(wav, w);
#if defined(_WIN32)
  std::printf("# AAC in CAF: no converter on this system; skipped\n");
  return;
#else
#if defined(__APPLE__)
  const std::string cmd = "/usr/bin/afconvert -f caff -d aac -b 128000 \"" + wav + "\" \"" + caf + "\" >/dev/null 2>&1";
  const bool made = std::system(cmd.c_str()) == 0 && fs::exists(caf);
#else
  // FFmpeg encodes the AAC (as a raw stream; its encoder primes 1024 frames); the CAF around it is ours.
  const std::string aac = dir + "/tone.adts";
  const std::string cmd = "ffmpeg -nostdin -y -hide_banner -loglevel error -i \"" + wav + "\" -c:a aac -b:a 128k -f adts \"" + aac + "\" >/dev/null 2>&1";
  const bool made = std::system(cmd.c_str()) == 0 && fs::exists(aac);
  if (made) writeFile(caf, cafFromAdts(readFile(aac), frames, 1024));
#endif
  if (!made) {
    std::printf("# AAC in CAF: no converter on this system; skipped\n");
    return;
  }
#endif
  const Bytes b = readFile(caf);
  caf::Info ci;
  expect(caf::parse(b.data(), b.size(), ci, nullptr) && ci.aac() && ci.hasPakt && ci.validFrames == frames, "caf aac: packet table states the length");
  const Bytes adts = caf::toAdts(b.data(), b.size(), ci);
  expect(adts.size() > ci.packetSizes.size() * 7 && adts[0] == 0xFF && (adts[1] & 0xF6) == 0xF0, "caf aac: packets as a raw AAC stream");
  SamplePtr smp = loadSample(caf);
  expect(smp && smp->frames() == frames, "caf aac: decodes to the stated length (priming and padding trimmed)");
  // Lossy, so compare the level and the alignment: the tone's peak lands where it should.
  double peak = 0;
  if (smp)
    for (size_t f = 1000; f < frames - 1000; ++f) peak = std::max(peak, static_cast<double>(std::fabs(smp->left[f])));
  expect(peak > 0.3 && peak < 0.45, "caf aac: the tone's level survives");
  // The raw AAC stream decodes by itself too (the route for decoders without the container).
  const std::string raw = dir + "/tone.aac";
  writeFile(raw, adts);
  DecodedAudio d;
  const bool rawOk = decodeAacFile(raw, d, nullptr);
  caf::trimToPacketTable(d, ci);
  expect(rawOk && d.frames() == frames, "caf aac: the rebuilt stream decodes and trims to the stated length");
}

// AIFF "basc" and WAV "acid" loops.
void testAiffWav() {
  const std::string dir = gl::test::hermeticTestEnv("loopfiles") + "/chunks";
  fs::create_directories(dir);
  // AIFF: 1 s mono 16-bit, 2 beats, root A (57), minor (scale 1), 4/4.
  Bytes comm;
  put16(comm, 1), put32(comm, 44100), put16(comm, 16);
  const uint8_t rate[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
  comm.insert(comm.end(), rate, rate + 10);
  Bytes basc;
  put32(basc, 1), put32(basc, 2), put16(basc, 57), put16(basc, 1), put16(basc, 4), put16(basc, 4), put16(basc, 1);
  basc.resize(84, 0);
  Bytes ssnd;
  put32(ssnd, 0), put32(ssnd, 0);
  for (int f = 0; f < 44100; ++f) put16(ssnd, static_cast<uint16_t>(static_cast<int16_t>((f % 100) * 50)));
  Bytes inner;
  tag(inner, "AIFF");
  chunk(inner, "COMM", comm);
  chunk(inner, "basc", basc);
  chunk(inner, "SSND", ssnd);
  Bytes aiff;
  tag(aiff, "FORM"), put32(aiff, static_cast<uint32_t>(inner.size())), aiff.insert(aiff.end(), inner.begin(), inner.end());
  const std::string ap = dir + "/loop.aif";
  writeFile(ap, aiff);
  loopfile::Info li;
  expect(loopfile::read(ap, li) && li.format == loopfile::Format::Aiff && li.loop && std::fabs(li.bpm - 120) < 1e-6 && li.key == 9 && li.minor,
         "aiff loop chunk: beats, tempo, key");
  SamplePtr s = loadSample(ap);
  expect(s && s->frames() == 44100, "aiff loop: still plays as AIFF");
  // WAV acid: one-shot flag off, 4 beats, tempo 95.
  Bytes w;
  tag(w, "RIFF");
  const size_t sizeAt = w.size();
  le32(w, 0), tag(w, "WAVE"), tag(w, "fmt "), le32(w, 16), le16(w, 1), le16(w, 1), le32(w, 44100), le32(w, 88200), le16(w, 2), le16(w, 16);
  tag(w, "acid"), le32(w, 24), le32(w, 2), le16(w, 62), le16(w, 0), le32(w, 0), le32(w, 4), le16(w, 4), le16(w, 4);
  float t = 95.0f;
  uint32_t tb;
  std::memcpy(&tb, &t, 4);
  le32(w, tb);
  tag(w, "data"), le32(w, 1000);
  w.resize(w.size() + 1000, 0);
  const uint32_t riff = static_cast<uint32_t>(w.size() - 8);
  for (int k = 0; k < 4; ++k) w[sizeAt + static_cast<size_t>(k)] = static_cast<uint8_t>(riff >> (8 * k));
  const std::string wp = dir + "/acid.wav";
  writeFile(wp, w);
  expect(loopfile::read(wp, li) && li.format == loopfile::Format::Wav && li.loop && li.bpm == 95 && li.beats == 4 && li.key == 2,
         "wav acid chunk: loop, beats, tempo, root note");
}

// ------------------------------------------- loop files into a project

void testSlicedImport() {
  const std::string dir = gl::test::hermeticTestEnv("loopfiles") + "/import";
  fs::create_directories(dir);
  const std::string path = dir + "/Break 120.rx2";
  writeFile(path, makeRex(basicSpec()));
  // Sample cache and browser.
  SamplePtr smp = loadSample(path);
  expect(smp && smp->frames() == basicSpec().pcm.size() && isAudioFile(path), "rx2: loads as a sample");
  bool listed = false;
  for (const auto& e : audioFileExtensions()) listed = listed || e == ".rx2";
  expect(listed, "rx2: in the audio file types");
  browse::FileMeta m = browse::Index::describe(path, true);
  expect(std::fabs(m.bpm - 120) < 1e-6 && m.slices == 4 && std::find(m.fileTags.begin(), m.fileTags.end(), "rex") != m.fileTags.end(),
         "browser: sliced loop tempo, slices and tags");
  expect(browse::matches(m, browse::parseQuery("tag:rex bpm:119-121")), "browser: search finds the sliced loop");
  // Slicer state.
  loopfile::Info li;
  expect(loopfile::read(path, li) && li.slices.size() == 4 && li.loop, "loop facts: a sliced loop");
  loopimport::Sliced sl;
  expect(loopimport::slicerFor(path, li, sl) && sl.state.type == "gl.slicer" && sl.state.data == path, "slicer for a sliced loop");
  const double frames = static_cast<double>(li.frames);
  const auto& P = sl.state.params;
  expect(std::fabs(P[pp::slMarker(0)] - 300 / frames) < 1e-6 && std::fabs(P[pp::slMarker(2)] - 44400 / frames) < 1e-6 && P[pp::slMarker(4)] < 0,
         "slicer: markers at the file's slices");
  expect(P[pp::SlMode] == pp::SlModeManual && P[pp::slParam(1, pp::SliceMute)] == 1 && std::fabs(P[pp::slParam2(2, pp::SliceEnd)] - 54400 / frames) < 1e-6,
         "slicer: manual slicing, muted slice, gated end");
  // Pattern notes on the beats (96 ticks per quarter): muted slice left out.
  const auto notes = loopimport::sliceNotes(sl, li, 96);
  expect(notes.size() == 3 && notes[0].start == 0 && notes[1].start == 192 && notes[2].start == 288 && notes[1].pitch == 62,
         "slicer: one note per playing slice on its beat");
  // Into a song: channel, pattern, clip.
  Song s;
  s.ppq = 96;
  if (s.mixer.empty()) s.mixer.resize(1);
  const auto r = loopimport::addSlicedLoop(s, path, li, 3, 384);
  const Clip* c = r.clip != kNoId ? s.clip(r.clip) : nullptr;
  expect(r.channel != kNoId && s.channel(r.channel) && s.channel(r.channel)->name == "Break 120" && s.pattern(r.pattern) &&
             s.pattern(r.pattern)->notes.at(r.channel).size() == 3,
         "song: slicer channel and its pattern");
  expect(c && c->track == 3 && c->start == 384 && c->length == 384 && c->pattern == r.pattern, "song: a pattern clip one loop long");
  // 7/8: beats are eighths, so the loop is 3.5 quarters long.
  loopfile::Info l78 = li;
  l78.sigNum = 7, l78.sigDen = 8, l78.beats = 7;
  expect(loopimport::loopTicks(l78, 96) == 336 && loopimport::beatTicks(l78, 2, 96) == 96, "meter: eighth-note beats");
  // More slices than settings: the region engine; more than keys: joined.
  loopfile::Info many = li;
  many.slices.clear();
  for (int k = 0; k < 100; ++k) many.slices.push_back(loopfile::Slice{k * 800, 800, k * 0.04, false});
  many.frames = 80000;
  expect(loopimport::slicerFor(path, many, sl) && sl.slices.size() == 100 && sl.state.params[pp::SlEngine] == 1 && sl.keys.front() == 28 &&
             sl.keys.back() == 127,
         "slicer: 100 slices on the region engine, every one on a key");
  slicex::Model md;
  expect(slicex::modelOf(sl.state.data, md) && md.decks[0].regions.size() == 100 && md.decks[0].regions[99].start == 99 * 800 &&
             md.decks[0].regions[99].key == 127 && slicex::sampleOf(sl.state.data) == path,
         "slicer: one region per slice with its key");
  many.slices.clear();
  int64_t at = 0;
  for (int k = 0; k < 500; ++k) {
    const int64_t len = k % 3 ? 800 : 400;
    many.slices.push_back(loopfile::Slice{at, len, static_cast<double>(at) / 22050.0, false});
    at += len;
  }
  many.frames = at;
  expect(loopimport::slicerFor(path, many, sl) && sl.slices.size() == 128 && sl.joined == 372 && sl.keys.front() == 0,
         "slicer: 500 slices joined down to the 128 keys");
  bool contiguous = true;
  for (size_t k = 1; k < sl.slices.size(); ++k) contiguous = contiguous && sl.slices[k - 1].start + sl.slices[k - 1].length == sl.slices[k].start;
  expect(contiguous, "slicer: joined slices still cover the loop end to end");
  // A loop that states its tempo follows the song's, pitch kept.
  PluginState smpState = defaultPluginState("gl.sampler");
  loopimport::followTempo(smpState, 97.5);
  expect(smpState.params[pp::SmpFitTempo] == 97.5f && smpState.params[pp::SmpStretchMode] == 1, "sampler follows the song tempo from the loop's own");
}

// Public-domain reference loops, when given (never part of the repository).
void testOracle() {
  const char* dir = std::getenv("GRIDLINE_LOOP_ORACLE_DIR");
  if (!dir || !*dir) {
    std::printf("# reference loops: GRIDLINE_LOOP_ORACLE_DIR not set; skipped\n");
    return;
  }
  const std::string d = dir;
  SamplePtr s = loadSample(d + "/120Mono.rx2");
  // The format notes' published values: frames 322.. of this file.
  const int want[10] = {-231, -421, -410, -209, 205, 564, 709, 585, 161, -349};
  bool same = s && s->frames() == 117760;
  for (int k = 0; same && k < 10; ++k) same = std::lround(s->left[322 + static_cast<size_t>(k)] * 32768.0f) == want[k];
  expect(same, "reference: 120Mono.rx2 decodes to the published samples");
  for (const char* f : {"120Stereo.rx2", "120Mono24Bits.rx2", "120SevenEights.rx2", "240FiveHundredSlices.rx2", "450OneHundredBars.rx2",
                        "100WeirdSampleRate.rx2", "120RexTest.rex", "120RcyTest.rcy"}) {
    SamplePtr x = loadSample(d + "/" + f);
    loopfile::Info li;
    loopfile::read(d + "/" + f, li);
    std::printf("# %s: %zu frames, %.3f BPM, %.2f beats, %zu slices\n", f, x ? x->frames() : 0, li.bpm, li.beats, li.slices.size());
    expect(x && x->frames() > 0, (std::string("reference: ") + f + " loads").c_str());
  }
  for (const char* f : {"ErrorCorrupt.rx2", "ErrorCorrupt2.rx2"}) {
    std::string err;
    SamplePtr x = loadSample(d + "/" + f, &err);
    expect(!x || x->frames() > 0, (std::string("reference: ") + f + " fails cleanly").c_str());
  }
}

}  // namespace

// The slicer's tempo readout: over the loop region of a file that has audio past its loop end.
void testKerfTempo() {
  const std::string dir = gl::test::hermeticTestEnv("loopfiles") + "/kerf";
  fs::create_directories(dir);
  RexSpec s = basicSpec();
  gSeed = 7;
  s.pcm = material(300 + 88200 + 3328, 1, 16);  // a tail after the loop end
  const std::string path = dir + "/Tail Break.rx2";
  writeFile(path, makeRex(s));
  SamplePtr smp = loadSample(path);
  expect(smp && smp->frames() == s.pcm.size(), "kerf tempo: the loop with a tail loads");
  if (!smp) return;
  const int64_t all = static_cast<int64_t>(smp->frames());
  const int64_t len = loopfile::loopFramesOf(smp->path, all);
  expect(len == 88200, "kerf tempo: the loop region's length");
  const double secs = static_cast<double>(len) / smp->sampleRate;
  const double bpm = kerfmarks::loopBeats(secs, false, 4) * 60.0 / secs;
  const double fitted = kerfmarks::loopBeats(secs, true, 4) * 60.0 / secs;
  const double whole = kerfmarks::loopBeats(smp->seconds(), false, 4) * 60.0 / smp->seconds();
  std::printf("# kerf tempo: %.2f BPM over the loop region, %.2f over the whole file\n", bpm, whole);
  expect(std::fabs(bpm - 120.0) < 0.05 && std::fabs(fitted - 120.0) < 0.05, "kerf tempo: 120.0 BPM over the loop region");
  expect(std::fabs(whole - 120.0) > 1.0, "kerf tempo: the whole file would read another tempo");
  expect(loopfile::loopFramesOf(smp->path, all) == 88200, "kerf tempo: the region is kept per file");
  // Files without a loop region, and files that are not there: all of their frames.
  const std::string plain = dir + "/plain.bin";
  writeFile(plain, Bytes(64, 0));
  expect(loopfile::loopFramesOf(plain, 5000) == 5000, "kerf tempo: no region, the whole sample");
  expect(loopfile::loopFramesOf(dir + "/missing.rx2", 5000) == 5000, "kerf tempo: a missing file, the whole sample");
  loopfile::Info li;
  li.loopStart = 100, li.loopEnd = 9000;
  expect(loopfile::loopFrames(li, 5000) == 4900, "kerf tempo: a region past the end stops at the end");
  li.loopStart = 6000;
  expect(loopfile::loopFrames(li, 5000) == 5000, "kerf tempo: a region outside the audio is ignored");
  expect(loopfile::loopFrames(li, 0) == 0, "kerf tempo: no audio, no length");
}

int main() {
  gl::test::hermeticTestEnv("loopfiles");
  testDwopKnownBits();
  testDwopRoundTrip();
  testRexContainer();
  testRexDamage();
  testCaf();
  testCafAac();
  testAiffWav();
  testSlicedImport();
  testKerfTempo();
  testOracle();
  return finish();
}

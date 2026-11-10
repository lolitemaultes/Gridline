// Gridline - sliced loop files (.rx2). See Rex2.h.
//
// Container: IFF style, big-endian sizes, chunks padded to even offsets;
// "CAT " lists carry a 4-byte type and nest. Chunks used: GLOB (loop
// settings), SLCE (slice entries, inside "CAT SLCL"),
// SINF (audio format and loop region), SDAT (compressed audio). Everything
// else is skipped.
//
// Audio: one adaptive predictive bitstream for the whole loop. Each coded
// channel keeps five integrated differences (orders 0 to 4) of the doubled
// sample and a running magnitude for each; every sample uses the order whose
// magnitude is smallest. A residual is coded as a run of zero bits (each one
// adds a step to the prefix; the step grows fourfold every seven zeros), a
// one bit, and a remainder below the final step in truncated binary. Stereo
// codes the left channel and the difference right minus left, with one
// shared bit reader.
#include "Rex2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gl::rex {

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

bool fail(std::string* err, const char* why) {
  if (err) *err = why;
  return false;
}

constexpr uint32_t kHeadMagic = 0x490cf18du;

struct Walk {
  Info* in = nullptr;
  bool head = false, sinf = false;
  int depth = 0;
};

// Walks the chunks in [p, p + n). False on a size that runs past its parent.
bool walk(const uint8_t* base, size_t at, size_t end, Walk& w) {
  if (++w.depth > 8) return false;  // nesting this deep is not a real file
  while (at + 8 <= end) {
    const uint8_t* c = base + at;
    const uint32_t size = be32(c + 4);
    const size_t body = at + 8;
    if (size > end - body) {
      --w.depth;
      return false;
    }
    const uint8_t* d = base + body;
    Info& in = *w.in;
    if (std::memcmp(c, "CAT ", 4) == 0 || std::memcmp(c, "FORM", 4) == 0) {
      if (size >= 4 && !walk(base, body + 4, body + size, w)) {
        --w.depth;
        return false;
      }
    } else if (std::memcmp(c, "HEAD", 4) == 0) {
      if (size >= 4 && be32(d) == kHeadMagic) w.head = true;
    } else if (std::memcmp(c, "GLOB", 4) == 0) {
      if (size >= 20) {
        in.entryCount = be32(d);
        in.bars = be16(d + 4);
        in.beats = d[6];
        in.sigNum = d[7];
        in.sigDen = d[8];
        in.sensitivity = d[9];
        in.gateSensitivity = be16(d + 10);
        in.gain = be16(d + 12);
        in.tempoMilli = be32(d + 16);
      }
    } else if (std::memcmp(c, "SLCE", 4) == 0) {
      if (size >= 11 && in.entries.size() < (1u << 16)) {
        SliceEntry e;
        e.start = be32(d);
        e.length = be32(d + 4);
        e.points = be16(d + 8);
        e.flags = d[10];
        in.entries.push_back(e);
      }
    } else if (std::memcmp(c, "SINF", 4) == 0) {
      if (size >= 10) {
        in.channels = d[0];
        in.formatCode = d[1];
        in.sampleRate = be32(d + 2);
        in.frames = be32(d + 6);
        if (size >= 18) {
          in.loopStart = be32(d + 10);
          in.loopEnd = be32(d + 14);
        }
        w.sinf = true;
      }
    } else if (std::memcmp(c, "SDAT", 4) == 0 || std::memcmp(c, "DWOP", 4) == 0) {
      in.audioOffset = body;
      in.audioSize = size;
      in.hasAudio = true;
    }
    at = body + size + (size & 1u);
  }
  --w.depth;
  return true;
}

// MSB-first reader over big-endian 32-bit words; zeros past the end.
struct BitReader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;  // next byte
  uint32_t word = 0;
  int left = 0;    // bits left in `word`
  uint64_t used = 0;  // bits read so far
  bool overrun = false;  // a bit past the end was read
  BitReader(const uint8_t* d, size_t sz) : p(d), n(sz) {}
  void load() {
    uint32_t w = 0;
    for (int k = 0; k < 4; ++k) {
      w <<= 8;
      if (pos < n) w |= p[pos];
      ++pos;
    }
    word = w;
    left = 32;
  }
  uint32_t bit() {
    if (left == 0) load();
    const uint32_t b = word >> 31;
    word <<= 1;
    --left;
    if (++used > static_cast<uint64_t>(n) * 8u) overrun = true;
    return b;
  }
  uint32_t bits(int count) {
    uint32_t v = 0;
    for (int k = 0; k < count; ++k) v = (v << 1) | bit();
    return v;
  }
};

struct Channel {
  int32_t d[5] = {0, 0, 0, 0, 0};
  uint32_t avg[5] = {2560, 2560, 2560, 2560, 2560};
  uint32_t j = 2;
  int rbits = 0;
};

// One coded value (doubled domain). False when the stream is not valid.
bool decodeOne(Channel& ch, BitReader& br, int32_t& out) {
  int idx = 0;
  for (int k = 1; k < 5; ++k)
    if (ch.avg[k] < ch.avg[idx]) idx = k;
  uint64_t step = ((static_cast<uint64_t>(ch.avg[idx]) * 3u) + 36u) >> 7;
  uint64_t prefix = 0;
  int window = 7;
  int zeros = 0;
  while (br.bit() == 0) {
    prefix += step;
    if (--window == 0) {
      step <<= 2;
      window = 7;
    }
    if (++zeros > 160 || br.overrun) return false;  // the step has long passed any 32-bit value
  }
  if (step > 0xFFFFFFFFull) return false;
  if (step < ch.j) {
    for (uint32_t jt = ch.j >> 1; step < jt; jt >>= 1) {
      ch.j = jt;
      --ch.rbits;
    }
  } else {
    while (step >= ch.j) {
      if (ch.j >= 0x80000000u) return false;
      ch.j <<= 1;
      ++ch.rbits;
    }
  }
  uint64_t rem = ch.rbits > 0 ? br.bits(ch.rbits) : 0;
  const uint64_t threshold = static_cast<uint64_t>(ch.j) - step;
  if (rem >= threshold) rem = rem * 2 - threshold + br.bit();
  const uint64_t code64 = prefix + rem;
  if (code64 > 0xFFFFFFFFull) return false;
  const uint32_t code = static_cast<uint32_t>(code64);
  // Even codes are non-negative residuals, odd codes negative ones.
  const int32_t r = static_cast<int32_t>((0u - (code & 1u)) ^ code);
  int32_t* d = ch.d;
  // Wrapping integer arithmetic, as a fixed-width decoder does (unsigned to
  // keep it defined): only damaged data ever wraps.
  auto add = [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); };
  auto sub = [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); };
  int32_t s = 0;
  switch (idx) {
    case 0: {
      const int32_t t0 = sub(r, d[0]), t1 = sub(t0, d[1]), t2 = sub(t1, d[2]);
      d[4] = sub(t2, d[3]), d[3] = t2, d[2] = t1, d[1] = t0, d[0] = r;
      s = r;
      break;
    }
    case 1: {
      const int32_t t1 = sub(r, d[1]), t2 = sub(t1, d[2]);
      s = add(d[0], r);
      d[4] = sub(t2, d[3]), d[3] = t2, d[2] = t1, d[1] = r, d[0] = s;
      break;
    }
    case 2: {
      const int32_t n1 = add(d[1], r);
      s = add(d[0], n1);
      const int32_t t = sub(r, d[2]);
      d[4] = sub(t, d[3]), d[3] = t, d[2] = r, d[1] = n1, d[0] = s;
      break;
    }
    case 3: {
      const int32_t n2 = add(d[2], r), n1 = add(d[1], n2);
      s = add(d[0], n1);
      d[4] = sub(r, d[3]), d[3] = r, d[2] = n2, d[1] = n1, d[0] = s;
      break;
    }
    default: {
      const int32_t n3 = add(d[3], r), n2 = add(d[2], n3), n1 = add(d[1], n2);
      s = add(d[0], n1);
      d[4] = r, d[3] = n3, d[2] = n2, d[1] = n1, d[0] = s;
      break;
    }
  }
  // Running magnitudes: ones' complement magnitude (-1 counts 0), decaying by 1/32.
  for (int k = 0; k < 5; ++k) {
    const uint32_t mag = static_cast<uint32_t>(d[k] ^ (d[k] >> 31));
    ch.avg[k] = ch.avg[k] + mag - (ch.avg[k] >> 5);
  }
  out = s;
  return true;
}

int32_t clampBits(int64_t v, int bits) {
  const int64_t hi = (int64_t{1} << (bits - 1)) - 1, lo = -(int64_t{1} << (bits - 1));
  return static_cast<int32_t>(std::clamp(v, lo, hi));
}

}  // namespace

bool looksLikeRex2(const uint8_t* p, size_t n) { return n >= 12 && std::memcmp(p, "CAT ", 4) == 0 && std::memcmp(p + 8, "REX2", 4) == 0; }

bool parse(const uint8_t* p, size_t n, Info& out, std::string* err) {
  out = Info{};
  if (!looksLikeRex2(p, n)) return fail(err, "not a sliced loop file");
  Walk w;
  w.in = &out;
  const uint32_t size = be32(p + 4);
  const size_t end = std::min<size_t>(n, 8 + static_cast<size_t>(size));
  if (!walk(p, 12, end, w)) return fail(err, "damaged chunk list");
  if (!w.head) return fail(err, "missing file header");
  if (!w.sinf) return fail(err, "missing audio format");
  if (out.channels != 1 && out.channels != 2) return fail(err, "unsupported channel count");
  if (out.formatCode == 3) out.bits = 16;
  else if (out.formatCode == 5) out.bits = 24;
  else return fail(err, "unsupported sample format");
  if (out.sampleRate < 1000 || out.sampleRate > 768000) return fail(err, "bad sample rate");
  // An hour at the highest rate: anything longer is damage, not a loop.
  if (out.frames == 0 || out.frames > 768000u * 3600u) return fail(err, "bad length");
  if (!out.hasAudio) return fail(err, "missing audio");
  std::stable_sort(out.entries.begin(), out.entries.end(), [](const SliceEntry& a, const SliceEntry& b) { return a.start < b.start; });
  return true;
}

bool decodePcm(const uint8_t* data, size_t size, int channels, int bits, uint32_t frames, std::vector<int32_t>& out, std::string* err) {
  if (channels != 1 && channels != 2) return fail(err, "unsupported channel count");
  if (bits != 16 && bits != 24) return fail(err, "unsupported sample format");
  out.assign(static_cast<size_t>(frames) * static_cast<size_t>(channels), 0);
  BitReader br(data, size);
  Channel c0, c1;
  int32_t* o = out.data();
  for (uint32_t f = 0; f < frames; ++f) {
    if ((f & 0xFFFFu) == 0 && decodeCancelled()) return fail(err, "cancelled");
    int32_t a = 0;
    if (!decodeOne(c0, br, a)) return fail(err, "damaged audio data");
    if (channels == 1) {
      *o++ = clampBits(a >> 1, bits);
    } else {
      int32_t b = 0;
      if (!decodeOne(c1, br, b)) return fail(err, "damaged audio data");
      *o++ = clampBits(a >> 1, bits);
      *o++ = clampBits((static_cast<int64_t>(a) + b) >> 1, bits);
    }
    if (br.overrun) return fail(err, "audio data ends early");
  }
  return true;
}

bool decode(const uint8_t* p, size_t n, DecodedAudio& out, Info* info, std::string* err) {
  out = DecodedAudio{};
  Info in;
  if (!parse(p, n, in, err)) return false;
  std::vector<int32_t> pcm;
  if (!decodePcm(p + in.audioOffset, in.audioSize, in.channels, in.bits, in.frames, pcm, err)) return false;
  out.channels = in.channels;
  out.sampleRate = static_cast<int>(in.sampleRate);
  out.bitsPerSample = in.bits;
  out.interleaved.resize(pcm.size());
  const float scale = in.bits == 24 ? 1.0f / 8388608.0f : 1.0f / 32768.0f;
  for (size_t k = 0; k < pcm.size(); ++k) out.interleaved[k] = static_cast<float>(pcm[k]) * scale;
  if (info) *info = std::move(in);
  return true;
}

void loopRegion(const Info& in, uint32_t& start, uint32_t& end) {
  start = 0, end = in.frames;
  if (in.loopEnd > in.loopStart && in.loopEnd <= in.frames) start = in.loopStart, end = in.loopEnd;
}

double loopBeats(const Info& in) {
  const int den = in.sigDen > 0 ? in.sigDen : 4;
  const int num = in.sigNum > 0 ? in.sigNum : 4;
  const double fromSettings = static_cast<double>(in.bars) * num + in.beats;
  if (fromSettings > 0) return fromSettings;
  // No bar count: from the tempo (in quarter notes) and the loop's length.
  uint32_t a = 0, b = 0;
  loopRegion(in, a, b);
  const uint32_t milli = in.tempoMilli;
  if (milli < 20000 || milli > 999000 || in.sampleRate == 0) return 0;
  const double quarters = static_cast<double>(b - a) / in.sampleRate * (milli / 1000.0) / 60.0;
  return quarters * den / 4.0;
}

double tempo(const Info& in) {
  // The audio's own tempo: the loop's beats over its length (the stored
  // preview tempo is only the speed it was last auditioned at).
  uint32_t a = 0, b = 0;
  loopRegion(in, a, b);
  const double beats = loopBeats(in);
  const int den = in.sigDen > 0 ? in.sigDen : 4;
  if (beats > 0 && b > a && in.sampleRate > 0) {
    const double quarters = beats * 4.0 / den;
    const double bpm = quarters * 60.0 * in.sampleRate / static_cast<double>(b - a);
    if (bpm >= 20 && bpm <= 999) return bpm;
  }
  if (in.tempoMilli >= 20000 && in.tempoMilli <= 999000) return in.tempoMilli / 1000.0;
  return 0;
}

double previewTempo(const Info& in) { return in.tempoMilli >= 20000 && in.tempoMilli <= 999000 ? in.tempoMilli / 1000.0 : 0; }

std::vector<Slice> slices(const Info& in) {
  std::vector<Slice> v;
  uint32_t a = 0, b = 0;
  loopRegion(in, a, b);
  // Stored slices inside the loop region (analysis markers are not slices).
  for (const SliceEntry& e : in.entries) {
    if (e.marker() || e.start < a || e.start >= b) continue;
    if (!v.empty() && e.start <= v.back().start) continue;
    v.push_back(Slice{e.start, e.length, e.muted()});
  }
  if (v.empty() || v.front().start > a) v.insert(v.begin(), Slice{a, 0, false});
  for (size_t k = 0; k < v.size(); ++k) {
    const uint32_t next = k + 1 < v.size() ? v[k + 1].start : std::max(b, v[k].start + 1);
    const uint32_t room = std::min(next, in.frames) - v[k].start;
    v[k].length = v[k].length > 1 ? std::min(v[k].length, room) : room;
  }
  return v;
}

double beatOf(const Info& in, uint32_t frame) {
  uint32_t a = 0, b = 0;
  loopRegion(in, a, b);
  const double beats = loopBeats(in);
  if (b <= a || beats <= 0) return 0;
  return (static_cast<double>(frame) - a) * beats / static_cast<double>(b - a);
}

}  // namespace gl::rex

// Gridline — WavPack decoder (lossless, float, packed 32-bit, hybrid). See WavPack.h.
//
// A .wv file is a sequence of blocks ("wvpk" + 32-byte header) that each hold
// a run of samples for one or two channels, plus metadata sub-blocks: the
// decorrelation filter (terms, weights, history), the adaptive entropy coder
// state (three running medians per channel) and the bitstream of residuals.
// Decoding = read residuals with the median-driven adaptive Golomb-style code,
// undo the cascade of decorrelation filters in reverse order, undo mid/side,
// then check the block checksum. Hybrid (lossy) blocks code each residual only
// to within an error limit driven by a bitrate profile; float and packed
// 32-bit blocks code integers and restore the rest from side information plus
// an optional "extra bits" stream with its own checksum.
#include "WavPack.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace gl {

namespace {

constexpr uint32_t kBytesStored = 3, kMono = 4, kHybrid = 8, kJoint = 0x10, kFloat = 0x80, kInt32 = 0x100,
                   kHybridBitrate = 0x200, kHybridBalance = 0x400, kInitial = 0x800, kFinal = 0x1000, kShiftLsb = 13,
                   kShiftMask = 0x1fu << 13, kRateLsb = 23, kRateMask = 0xfu << 23, kFalseStereo = 0x40000000u,
                   kDsd = 0x80000000u;
// Float information flags (which bits of each float the integer stream lost
// and how the extra stream restores them).
constexpr int kFloatShiftOnes = 1, kFloatShiftSame = 2, kFloatShiftSent = 4, kFloatZerosSent = 8, kFloatNegZeros = 0x10;
constexpr int kRates[15] = {6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 192000};
constexpr int kMaxTerm = 8;
constexpr int kLimitOnes = 16;

// 2^(i/256) scaled to 8 fractional bits, minus the implicit leading one.
const std::array<uint8_t, 256>& exp2Table() {
  static const std::array<uint8_t, 256> t = [] {
    std::array<uint8_t, 256> a{};
    for (int i = 0; i < 256; ++i) {
      long v = std::lround(256.0 * std::pow(2.0, i / 256.0)) - 256;
      a[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(v, 0L, 255L));
    }
    return a;
  }();
  return t;
}

// Inverse of the encoder's 8.8 fixed-point log2 representation.
int32_t exp2s(int log) {
  if (log < 0) return -exp2s(-log);
  uint32_t value = exp2Table()[static_cast<size_t>(log & 0xff)] | 0x100u;
  log >>= 8;
  if (log <= 9) return static_cast<int32_t>(value >> (9 - log));
  if (log >= 31) return 0x7fffffff;  // out of range for any valid stream; saturate
  return static_cast<int32_t>(std::min<uint64_t>(static_cast<uint64_t>(value) << (log - 9), 0x7fffffffu));
}

// log2(1 + i/256) scaled to 8 fractional bits.
const std::array<uint8_t, 256>& log2Table() {
  static const std::array<uint8_t, 256> t = [] {
    std::array<uint8_t, 256> a{};
    for (int i = 0; i < 256; ++i) {
      long v = std::lround(256.0 * std::log2(1.0 + i / 256.0));
      a[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(v, 0L, 255L));
    }
    return a;
  }();
  return t;
}

int countBits(uint32_t v);

// The encoder's 8.8 fixed-point log2 (used by the hybrid bitrate tracker).
int log2s(uint32_t v) {
  v += v >> 9;
  int n = countBits(v);
  uint32_t idx = n <= 9 ? v << (9 - n) : v >> (n - 9);
  return (n << 8) + log2Table()[idx & 0xff];
}

int restoreWeight(int8_t w) {
  int r = static_cast<int>(w) * 8;
  if (r > 0) r += (r + 64) >> 7;
  return r;
}

int32_t applyWeight(int32_t weight, int32_t sample) {
  if (sample == static_cast<int16_t>(sample)) return static_cast<int32_t>((static_cast<int64_t>(weight) * sample + 512) >> 10);
  // Wide samples: split product, rounded the same way the encoder does it.
  int64_t lo = (static_cast<int64_t>(sample & 0xffff) * weight) >> 9;
  int64_t hi = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(sample) & 0xffff0000u) >> 9) * weight;
  return static_cast<int32_t>((lo + hi + 1) >> 1);
}

void updateWeight(int32_t& weight, int delta, int32_t source, int32_t result) {
  if (source && result) weight += ((source ^ result) < 0) ? -delta : delta;
}
void updateWeightClip(int32_t& weight, int delta, int32_t source, int32_t result) {
  if (source && result) {
    if ((source ^ result) < 0) {
      weight -= delta;
      if (weight < -1024) weight = -1024;
    } else {
      weight += delta;
      if (weight > 1024) weight = 1024;
    }
  }
}

struct Bits {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;
  uint32_t sr = 0;
  int bc = 0;
  bool overrun = false;
  int bit() {
    if (bc == 0) {
      if (p >= end) {
        overrun = true;
        return 0;
      }
      sr = *p++;
      bc = 8;
    }
    int b = static_cast<int>(sr & 1u);
    sr >>= 1;
    --bc;
    return b;
  }
  uint32_t bits(int n) {  // first bit read is the least significant
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint32_t>(bit()) << i;
    return v;
  }
};

int countBits(uint32_t v) {
  int n = 0;
  while (v) ++n, v >>= 1;
  return n;
}

// A value in [0, maxcode] with a truncated binary code.
uint32_t readCode(Bits& bs, uint32_t maxcode) {
  if (maxcode < 2) return maxcode ? static_cast<uint32_t>(bs.bit()) : 0u;
  int bitcount = countBits(maxcode);
  uint32_t extras = (1u << bitcount) - maxcode - 1;
  uint32_t code = bs.bits(bitcount - 1);
  if (code >= extras) code = (code << 1) - extras + static_cast<uint32_t>(bs.bit());
  return code;
}

// Elias-gamma-like escape code used for long runs.
bool readEscape(Bits& bs, uint32_t& value) {
  int cbits = 0;
  while (cbits < 33 && bs.bit()) ++cbits;
  if (cbits == 33) return false;
  if (cbits < 2) {
    value = static_cast<uint32_t>(cbits);
    return true;
  }
  uint32_t v = 0, mask = 1;
  for (--cbits; cbits; mask <<= 1, --cbits)
    if (bs.bit()) v |= mask;
  value = v | mask;
  return true;
}

struct EntropyChan {
  uint32_t median[3] = {0, 0, 0};
  int32_t slowLevel = 0;    // hybrid bitrate mode: running average of log2(|residual|)
  uint32_t errorLimit = 0;  // hybrid: residuals are only coded to within this distance
};

struct Entropy {
  EntropyChan c[2];
  uint32_t zerosAcc = 0;
  int holdingOne = 0, holdingZero = 0;
  // Hybrid (lossy) state, from the hybrid profile metadata.
  bool hybrid = false, bitrateMode = false, balance = false;
  uint32_t bitrateAcc[2] = {0, 0};
  int32_t bitrateDelta[2] = {0, 0};
};

constexpr uint32_t kDiv0 = 128, kDiv1 = 64, kDiv2 = 32;
constexpr int kSlowShift = 8, kSlowRound = 1 << (kSlowShift - 1);
inline uint32_t getMed(const EntropyChan& c, int k) { return (c.median[k] >> 4) + 1; }
inline void incMed(EntropyChan& c, int k, uint32_t div) { c.median[k] += ((c.median[k] + div) / div) * 5; }
inline void decMed(EntropyChan& c, int k, uint32_t div) { c.median[k] -= ((c.median[k] + (div - 2)) / div) * 2; }
inline void decaySlow(EntropyChan& c) { c.slowLevel -= (c.slowLevel + kSlowRound) >> kSlowShift; }

uint32_t limitFromLevel(int slowLevel, int bitrate) {
  int slowLog = (slowLevel + kSlowRound) >> kSlowShift;
  return slowLog - bitrate > -0x100 ? static_cast<uint32_t>(exp2s(slowLog - bitrate + 0x100)) : 0u;
}

// Hybrid: once per frame, advance the target bitrate and derive how coarsely
// each channel's residuals were quantised.
void updateErrorLimit(Entropy& w, bool stereo) {
  w.bitrateAcc[0] += static_cast<uint32_t>(w.bitrateDelta[0]);
  int rate0 = static_cast<int32_t>(w.bitrateAcc[0]) >> 16;
  if (!stereo) {
    w.c[0].errorLimit = w.bitrateMode ? limitFromLevel(w.c[0].slowLevel, rate0) : static_cast<uint32_t>(exp2s(rate0));
    return;
  }
  w.bitrateAcc[1] += static_cast<uint32_t>(w.bitrateDelta[1]);
  int rate1 = static_cast<int32_t>(w.bitrateAcc[1]) >> 16;
  if (!w.bitrateMode) {
    w.c[0].errorLimit = static_cast<uint32_t>(exp2s(rate0));
    w.c[1].errorLimit = static_cast<uint32_t>(exp2s(rate1));
    return;
  }
  if (w.balance) {  // the second accumulator steers bits between the channels
    int slow0 = (w.c[0].slowLevel + kSlowRound) >> kSlowShift;
    int slow1 = (w.c[1].slowLevel + kSlowRound) >> kSlowShift;
    int bal = (slow1 - slow0 + rate1 + 1) >> 1;
    if (bal > rate0) {
      rate1 = rate0 * 2;
      rate0 = 0;
    } else if (-bal > rate0) {
      rate0 = rate0 * 2;
      rate1 = 0;
    } else {
      rate1 = rate0 + bal;
      rate0 = rate0 - bal;
    }
  }
  w.c[0].errorLimit = limitFromLevel(w.c[0].slowLevel, rate0);
  w.c[1].errorLimit = limitFromLevel(w.c[1].slowLevel, rate1);
}

// Reads `count` residuals (interleaved by channel when stereo).
bool readResiduals(Entropy& w, Bits& bs, int32_t* out, size_t count, bool stereo) {
  for (size_t i = 0; i < count; ++i) {
    if (w.hybrid && (!stereo || !(i & 1))) updateErrorLimit(w, stereo);
    EntropyChan& c = w.c[stereo ? (i & 1) : 0];
    // Silence: when every median is (nearly) zero, runs of zeros are coded as a length.
    if (!(w.c[0].median[0] & ~1u) && !w.holdingZero && !w.holdingOne && !(w.c[1].median[0] & ~1u)) {
      if (w.zerosAcc) {
        if (--w.zerosAcc) {
          decaySlow(c);
          out[i] = 0;
          continue;
        }
      } else {
        uint32_t z = 0;
        if (!readEscape(bs, z)) return false;
        w.zerosAcc = z;
        if (w.zerosAcc) {
          decaySlow(c);
          for (auto& ch : w.c) ch.median[0] = ch.median[1] = ch.median[2] = 0;
          out[i] = 0;
          continue;
        }
      }
    }
    uint32_t ones = 0;
    if (w.holdingZero) {
      ones = 0;
      w.holdingZero = 0;
    } else {
      while (ones < static_cast<uint32_t>(kLimitOnes + 1) && bs.bit()) ++ones;
      if (ones == static_cast<uint32_t>(kLimitOnes + 1)) return false;
      if (ones >= static_cast<uint32_t>(kLimitOnes)) {
        uint32_t extra = 0;
        if (!readEscape(bs, extra)) return false;
        ones = extra + kLimitOnes;
      }
      if (w.holdingOne) {
        w.holdingOne = static_cast<int>(ones & 1);
        ones = (ones >> 1) + 1;
      } else {
        w.holdingOne = static_cast<int>(ones & 1);
        ones >>= 1;
      }
      w.holdingZero = ~w.holdingOne & 1;
    }
    uint32_t low, high;
    if (ones == 0) {
      low = 0;
      high = getMed(c, 0) - 1;
      decMed(c, 0, kDiv0);
    } else {
      low = getMed(c, 0);
      incMed(c, 0, kDiv0);
      if (ones == 1) {
        high = low + getMed(c, 1) - 1;
        decMed(c, 1, kDiv1);
      } else {
        low += getMed(c, 1);
        incMed(c, 1, kDiv1);
        if (ones == 2) {
          high = low + getMed(c, 2) - 1;
          decMed(c, 2, kDiv2);
        } else {
          low += (ones - 2) * getMed(c, 2);
          high = low + getMed(c, 2) - 1;
          incMed(c, 2, kDiv2);
        }
      }
    }
    if (!w.hybrid) {
      low += readCode(bs, high - low);
      out[i] = bs.bit() ? static_cast<int32_t>(~low) : static_cast<int32_t>(low);
    } else {
      // Lossy: the residual is only known to lie in [low, high]; narrow the
      // interval by bisection until it is within the error limit, then take
      // its midpoint.
      low &= 0x7fffffffu;
      high &= 0x7fffffffu;
      if (low > high) high = low;
      uint32_t mid = (high + low + 1) >> 1;
      if (!c.errorLimit) {
        mid = readCode(bs, high - low) + low;
      } else {
        while (high - low > c.errorLimit) {
          if (bs.bit())
            low = mid;
          else
            high = mid - 1;
          mid = (high + low + 1) >> 1;
          if (bs.overrun) return false;
        }
      }
      out[i] = bs.bit() ? static_cast<int32_t>(~mid) : static_cast<int32_t>(mid);
      if (w.bitrateMode) {
        decaySlow(c);
        c.slowLevel += log2s(mid);
      }
    }
    if (bs.overrun) return false;
  }
  return true;
}

struct Pass {
  int term = 0, delta = 0;
  int32_t weightA = 0, weightB = 0;
  int32_t a[kMaxTerm] = {}, b[kMaxTerm] = {};
};

void monoPass(Pass& d, int32_t* buf, size_t n) {
  if (d.term == 17 || d.term == 18) {
    for (size_t i = 0; i < n; ++i) {
      int32_t sam = d.term == 17 ? 2 * d.a[0] - d.a[1] : (3 * d.a[0] - d.a[1]) >> 1;
      d.a[1] = d.a[0];
      d.a[0] = applyWeight(d.weightA, sam) + buf[i];
      updateWeight(d.weightA, d.delta, sam, buf[i]);
      buf[i] = d.a[0];
    }
    return;
  }
  int m = 0, k = d.term & (kMaxTerm - 1);
  for (size_t i = 0; i < n; ++i) {
    int32_t sam = d.a[m];
    d.a[k] = applyWeight(d.weightA, sam) + buf[i];
    updateWeight(d.weightA, d.delta, sam, buf[i]);
    buf[i] = d.a[k];
    m = (m + 1) & (kMaxTerm - 1);
    k = (k + 1) & (kMaxTerm - 1);
  }
}

void stereoPass(Pass& d, int32_t* buf, size_t frames) {
  int32_t* end = buf + frames * 2;
  switch (d.term) {
    case 17:
    case 18:
      for (int32_t* p = buf; p < end; p += 2) {
        int32_t sa = d.term == 17 ? 2 * d.a[0] - d.a[1] : (3 * d.a[0] - d.a[1]) >> 1;
        d.a[1] = d.a[0];
        d.a[0] = applyWeight(d.weightA, sa) + p[0];
        updateWeight(d.weightA, d.delta, sa, p[0]);
        p[0] = d.a[0];
        int32_t sb = d.term == 17 ? 2 * d.b[0] - d.b[1] : (3 * d.b[0] - d.b[1]) >> 1;
        d.b[1] = d.b[0];
        d.b[0] = applyWeight(d.weightB, sb) + p[1];
        updateWeight(d.weightB, d.delta, sb, p[1]);
        p[1] = d.b[0];
      }
      break;
    case -1:
      for (int32_t* p = buf; p < end; p += 2) {
        int32_t sa = p[0] + applyWeight(d.weightA, d.a[0]);
        updateWeightClip(d.weightA, d.delta, d.a[0], p[0]);
        p[0] = sa;
        d.a[0] = p[1] + applyWeight(d.weightB, sa);
        updateWeightClip(d.weightB, d.delta, sa, p[1]);
        p[1] = d.a[0];
      }
      break;
    case -2:
      for (int32_t* p = buf; p < end; p += 2) {
        int32_t sb = p[1] + applyWeight(d.weightB, d.b[0]);
        updateWeightClip(d.weightB, d.delta, d.b[0], p[1]);
        p[1] = sb;
        d.b[0] = p[0] + applyWeight(d.weightA, sb);
        updateWeightClip(d.weightA, d.delta, sb, p[0]);
        p[0] = d.b[0];
      }
      break;
    case -3:
      for (int32_t* p = buf; p < end; p += 2) {
        int32_t sa = p[0] + applyWeight(d.weightA, d.a[0]);
        updateWeightClip(d.weightA, d.delta, d.a[0], p[0]);
        int32_t sb = p[1] + applyWeight(d.weightB, d.b[0]);
        updateWeightClip(d.weightB, d.delta, d.b[0], p[1]);
        p[0] = d.b[0] = sa;
        p[1] = d.a[0] = sb;
      }
      break;
    default: {
      int m = 0, k = d.term & (kMaxTerm - 1);
      for (int32_t* p = buf; p < end; p += 2) {
        int32_t sa = d.a[m], sb = d.b[m];
        d.a[k] = applyWeight(d.weightA, sa) + p[0];
        d.b[k] = applyWeight(d.weightB, sb) + p[1];
        updateWeight(d.weightA, d.delta, sa, p[0]);
        updateWeight(d.weightB, d.delta, sb, p[1]);
        p[0] = d.a[k];
        p[1] = d.b[k];
        m = (m + 1) & (kMaxTerm - 1);
        k = (k + 1) & (kMaxTerm - 1);
      }
      break;
    }
  }
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
int16_t le16s(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

// One decoded block: `channels` channels of `frames` samples, already scaled to -1..1.
struct Block {
  int channels = 0;
  size_t frames = 0;
  int bits = 0;  // source resolution (32 for float and 32-bit integer data)
  std::vector<float> samples;  // interleaved
};

// How a float block's integers map back to IEEE floats (ID 0x8 metadata).
struct FloatInfo {
  int flags = 0, shift = 0, maxExp = 0, normExp = 127;
};

// How a 32-bit integer block's (at most 24-bit) integers map back (ID 0x9).
struct Int32Info {
  int sentBits = 0, zeros = 0, ones = 0, dups = 0;
};

// The "extra" bitstream (ID 0xc) carries what the integer coding dropped from
// float / 32-bit data in lossless files: a 32-bit checksum of the restored
// values followed by the bits themselves.
struct Extra {
  bool present = false;
  uint32_t crc = 0;
  Bits bs;
  int bit() { return present ? bs.bit() : 0; }
  uint32_t bits(int n) { return present ? bs.bits(n) : 0u; }
};

float fromBits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}

// Rebuilds IEEE floats. Each integer is the float's 24-bit mantissa (with the
// implicit one) aligned to the block's largest exponent, then shifted right by
// `shift`; normalising it back gives the exponent, and the bits shifted out on
// the way are restored from the extra stream (or filled per the flags).
bool restoreFloats(const int32_t* v, size_t n, const FloatInfo& fi, Extra& ex, bool lossy, float* out) {
  uint32_t crc = 0xffffffffu;
  const float scale = fi.normExp && fi.normExp != 127 ? std::ldexp(1.0f, 127 - fi.normExp) : 1.0f;
  for (size_t i = 0; i < n; ++i) {
    uint32_t mant = 0, expo = 0, sign = 0;
    if (v[i] == 0) {
      if (fi.flags & kFloatZerosSent) {  // values too small for the integer stream
        if (ex.bit()) {
          mant = ex.bits(23);
          if (fi.maxExp >= 25) expo = ex.bits(8);
          sign = static_cast<uint32_t>(ex.bit());
        } else if (fi.flags & kFloatNegZeros) {
          sign = static_cast<uint32_t>(ex.bit());
        }
      }
    } else {
      int64_t val = static_cast<int64_t>(v[i]) * (int64_t{1} << std::clamp(fi.shift, 0, 31));
      if (val < 0) sign = 1, val = -val;
      int e = fi.maxExp;
      if (!lossy && val == 0x1000000) {  // infinity / NaN
        if (ex.bit()) mant = ex.bits(23);
        expo = 255;
      } else {
        while (val >= 0x1000000) val >>= 1, ++e;  // only lossy data overshoots
        int lost = 0;
        if (e)
          while (!(val & 0x800000) && --e) ++lost, val <<= 1;
        if (lost) {
          const int64_t fill = (int64_t{1} << lost) - 1;
          if ((fi.flags & kFloatShiftOnes) || ((fi.flags & kFloatShiftSame) && ex.bit()))
            val |= fill;
          else if (fi.flags & kFloatShiftSent)
            val |= static_cast<int64_t>(ex.bits(lost)) & fill;
        }
        if (e >= 255) e = 254, val = 0xffffff;  // lossy overshoot: saturate
        mant = static_cast<uint32_t>(val) & 0x7fffffu;
        expo = static_cast<uint32_t>(e);
      }
    }
    crc = crc * 27 + mant * 9 + expo * 3 + sign;
    float f = fromBits(sign << 31 | expo << 23 | mant) * scale;
    out[i] = std::isfinite(f) ? f : 0.0f;  // a DAW has no use for inf/NaN samples
  }
  if (ex.present && (ex.bs.overrun || crc != ex.crc)) return false;
  return true;
}

uint32_t restoreInt32(uint32_t u, const Int32Info& ii, Extra& ex) {
  if (ii.sentBits) u = (u << ii.sentBits) | (ex.bits(ii.sentBits) & ((1u << ii.sentBits) - 1));
  if (ii.zeros)
    u <<= ii.zeros;
  else if (ii.ones)
    u = ((u + 1) << ii.ones) - 1;
  else if (ii.dups)
    u = ((u + (u & 1)) << ii.dups) - (u & 1);
  return u;
}

bool decodeBlock(const uint8_t* blk, uint32_t flags, uint32_t frames, uint32_t blockCrc, size_t blockEnd, Block& out,
                 int& rateOut, std::string* err) {
  if (flags & kDsd) return fail(err, "DSD WavPack is not supported");
  const bool monoData = (flags & kMono) || (flags & kFalseStereo);
  const bool hybrid = (flags & kHybrid) != 0;
  std::vector<Pass> passes;
  Entropy ent;
  ent.hybrid = hybrid;
  ent.bitrateMode = (flags & kHybridBitrate) != 0;
  ent.balance = (flags & kHybridBalance) != 0;
  Bits bs;
  Extra extra;
  FloatInfo floatInfo;
  Int32Info int32Info;
  bool haveBits = false, haveEntropy = false, haveProfile = false, haveFloat = false;
  const uint8_t* p = blk + 32;
  const uint8_t* end = blk + blockEnd;
  while (p + 2 <= end) {
    int id = p[0];
    size_t size, hdr;
    if (id & 0x80) {
      if (p + 4 > end) break;
      size = static_cast<size_t>(p[1] | (p[2] << 8) | (p[3] << 16)) * 2;
      hdr = 4;
    } else {
      size = static_cast<size_t>(p[1]) * 2;
      hdr = 2;
    }
    const uint8_t* d = p + hdr;
    if (d + size > end) return fail(err, "truncated WavPack metadata");
    size_t len = size - ((id & 0x40) && size ? 1 : 0);
    switch (id & 0x3f) {
      case 0x2: {  // decorrelation terms, stored last pass first
        passes.assign(len, Pass{});
        for (size_t i = 0; i < len; ++i) {
          Pass& ps = passes[len - 1 - i];
          ps.term = (d[i] & 0x1f) - 5;
          ps.delta = (d[i] >> 5) & 7;
          if (!ps.term || ps.term < -3 || (ps.term > kMaxTerm && ps.term < 17) || ps.term > 18 || (monoData && ps.term < 0))
            return fail(err, "bad WavPack decorrelation term");
        }
        break;
      }
      case 0x3: {  // weights, from the last pass backwards
        size_t n = monoData ? len : len / 2, k = 0;
        for (size_t i = passes.size(); i-- > 0 && n--;) {
          passes[i].weightA = restoreWeight(static_cast<int8_t>(d[k++]));
          if (!monoData) passes[i].weightB = restoreWeight(static_cast<int8_t>(d[k++]));
        }
        break;
      }
      case 0x4: {  // filter history, from the last pass backwards
        const uint8_t* q = d;
        const uint8_t* qe = d + len;
        for (size_t i = passes.size(); i-- > 0 && q < qe;) {
          Pass& ps = passes[i];
          if (ps.term > kMaxTerm) {
            if (q + (monoData ? 4 : 8) > qe) return fail(err, "bad WavPack history");
            ps.a[0] = exp2s(le16s(q)), ps.a[1] = exp2s(le16s(q + 2)), q += 4;
            if (!monoData) ps.b[0] = exp2s(le16s(q)), ps.b[1] = exp2s(le16s(q + 2)), q += 4;
          } else if (ps.term < 0) {
            if (q + 4 > qe) return fail(err, "bad WavPack history");
            ps.a[0] = exp2s(le16s(q)), ps.b[0] = exp2s(le16s(q + 2)), q += 4;
          } else {
            for (int m = 0; m < ps.term; ++m) {
              if (q + (monoData ? 2 : 4) > qe) return fail(err, "bad WavPack history");
              ps.a[m] = exp2s(le16s(q)), q += 2;
              if (!monoData) ps.b[m] = exp2s(le16s(q)), q += 2;
            }
          }
        }
        break;
      }
      case 0x5: {  // entropy coder medians
        if (len < (monoData ? 6u : 12u)) return fail(err, "bad WavPack entropy variables");
        for (int k = 0; k < 3; ++k) ent.c[0].median[k] = static_cast<uint32_t>(exp2s(d[2 * k] | (d[2 * k + 1] << 8)));
        if (!monoData)
          for (int k = 0; k < 3; ++k) ent.c[1].median[k] = static_cast<uint32_t>(exp2s(d[6 + 2 * k] | (d[6 + 2 * k + 1] << 8)));
        haveEntropy = true;
        break;
      }
      case 0x6: {  // hybrid profile: [slow levels], bitrate accumulators, [bitrate deltas]
        const size_t step = monoData ? 2 : 4;
        size_t k = 0;
        if (ent.bitrateMode) {
          if (len < k + step) return fail(err, "bad WavPack hybrid profile");
          ent.c[0].slowLevel = exp2s(d[k] | (d[k + 1] << 8));
          if (!monoData) ent.c[1].slowLevel = exp2s(d[k + 2] | (d[k + 3] << 8));
          k += step;
        }
        if (len < k + step) return fail(err, "bad WavPack hybrid profile");
        ent.bitrateAcc[0] = static_cast<uint32_t>(d[k] | (d[k + 1] << 8)) << 16;
        if (!monoData) ent.bitrateAcc[1] = static_cast<uint32_t>(d[k + 2] | (d[k + 3] << 8)) << 16;
        k += step;
        if (len >= k + step) {
          ent.bitrateDelta[0] = exp2s(le16s(d + k));
          if (!monoData) ent.bitrateDelta[1] = exp2s(le16s(d + k + 2));
        }
        haveProfile = true;
        break;
      }
      case 0x8:  // float information
        if (len < 4) return fail(err, "bad WavPack float information");
        floatInfo.flags = d[0];
        floatInfo.shift = d[1];
        floatInfo.maxExp = d[2];
        floatInfo.normExp = d[3];
        haveFloat = true;
        break;
      case 0x9:  // 32-bit integer information
        if (len < 4) return fail(err, "bad WavPack integer information");
        int32Info.sentBits = d[0];
        int32Info.zeros = d[1];
        int32Info.ones = d[2];
        int32Info.dups = d[3];
        if (int32Info.sentBits > 24 || int32Info.zeros > 31 || int32Info.ones > 31 || int32Info.dups > 31)
          return fail(err, "bad WavPack integer information");
        break;
      case 0xa:
        bs.p = d;
        bs.end = d + len;
        haveBits = true;
        break;
      case 0xc:  // extra bits for float / 32-bit data
        if (len >= 4) {
          extra.present = true;
          extra.crc = le32(d);
          extra.bs.p = d + 4;
          extra.bs.end = d + len;
        }
        break;
      case 0x27:  // non-standard sample rate
        if (len >= 3) rateOut = static_cast<int>(d[0] | (d[1] << 8) | (d[2] << 16) | (len >= 4 ? d[3] << 24 : 0));
        break;
      default: break;  // RIFF header/trailer, checksums, channel info, noise-shaping weights: not needed
    }
    p = d + size;
  }
  if (!haveBits || !haveEntropy) return fail(err, "WavPack block without audio data");
  if (hybrid && !haveProfile) return fail(err, "hybrid WavPack block without bitrate profile");
  if ((flags & kFloat) && !haveFloat) return fail(err, "float WavPack block without float information");
  const size_t count = static_cast<size_t>(frames) * (monoData ? 1 : 2);
  std::vector<int32_t> buf(count);
  if (!readResiduals(ent, bs, buf.data(), count, !monoData)) return fail(err, "corrupt WavPack bitstream");
  for (auto& ps : passes) {
    if (monoData)
      monoPass(ps, buf.data(), frames);
    else
      stereoPass(ps, buf.data(), frames);
  }
  if (!monoData && (flags & kJoint))
    for (size_t i = 0; i < count; i += 2) buf[i] += (buf[i + 1] -= (buf[i] >> 1));
  // The checksum covers the integers as decoded (for hybrid files: the lossy ones).
  uint32_t crc = 0xffffffffu;
  for (int32_t v : buf) crc = crc * 3 + static_cast<uint32_t>(v);
  if (crc != blockCrc) return fail(err, "WavPack checksum mismatch");
  const int shift = static_cast<int>((flags & kShiftMask) >> kShiftLsb);
  std::vector<float> f(count);
  if (flags & kFloat) {
    if (shift)
      for (auto& v : buf) v = static_cast<int32_t>(static_cast<uint32_t>(v) << shift);
    if (!restoreFloats(buf.data(), count, floatInfo, extra, hybrid, f.data()))
      return fail(err, "WavPack float checksum mismatch");
    out.bits = 32;
  } else if (flags & kInt32) {
    // The packing shrank the integers (trailing zeros / ones / duplicated
    // bits, low bits moved to the extra stream); the restored values are in
    // the stored width, which some encoders use for 16-bit data too.
    const int bits = static_cast<int>((flags & kBytesStored) + 1) * 8;
    const double scale = 1.0 / static_cast<double>(int64_t{1} << (bits - 1));
    uint32_t xcrc = 0xffffffffu;
    for (size_t i = 0; i < count; ++i) {
      uint32_t u = restoreInt32(static_cast<uint32_t>(buf[i]) << shift, int32Info, extra);
      xcrc = xcrc * 9 + (u & 0xffff) * 3 + ((u >> 16) & 0xffff);
      f[i] = static_cast<float>(static_cast<int32_t>(u) * scale);
    }
    if (extra.present && int32Info.sentBits && (extra.bs.overrun || xcrc != extra.crc))
      return fail(err, "WavPack extra-bits checksum mismatch");
    out.bits = bits;
  } else {
    const int bits = static_cast<int>((flags & kBytesStored) + 1) * 8;
    const int64_t hi = (int64_t{1} << (bits - 1)) - 1, lo = -hi - 1;
    const double scale = 1.0 / static_cast<double>(hi + 1);
    for (size_t i = 0; i < count; ++i) {
      int64_t v = static_cast<int64_t>(buf[i]) * (int64_t{1} << shift);
      if (hybrid) v = std::clamp(v, lo, hi);  // lossy reconstruction may overshoot full scale
      f[i] = static_cast<float>(static_cast<double>(v) * scale);
    }
    out.bits = bits;
  }
  out.frames = frames;
  if (flags & kFalseStereo) {  // mono-coded stereo
    out.channels = 2;
    out.samples.resize(count * 2);
    for (size_t i = 0; i < count; ++i) out.samples[2 * i] = out.samples[2 * i + 1] = f[i];
  } else {
    out.channels = monoData ? 1 : 2;
    out.samples = std::move(f);
  }
  return true;
}

}  // namespace

bool decodeWavPack(const uint8_t* data, size_t size, DecodedAudio& out, std::string* err) {
  out = DecodedAudio{};
  size_t pos = 0;
  std::vector<Block> group;  // blocks of one multichannel frame group (INITIAL … FINAL)
  int groupChannels = 0;
  while (pos + 32 <= size) {
    const uint8_t* b = data + pos;
    if (b[0] != 'w' || b[1] != 'v' || b[2] != 'p' || b[3] != 'k') {
      if (out.channels > 0) break;  // trailing junk (e.g. tags) after the audio
      return fail(err, "not a WavPack file");
    }
    uint32_t ck = le32(b + 4);
    if (ck < 24 || pos + 8 + ck > size) return fail(err, "truncated WavPack block");
    uint16_t version = static_cast<uint16_t>(b[8] | (b[9] << 8));
    if (version < 0x402 || version > 0x410) return fail(err, "unsupported WavPack version");
    uint32_t frames = le32(b + 20), flags = le32(b + 24), crc = le32(b + 28);
    pos += 8 + ck;
    if (frames == 0) continue;  // metadata-only block
    if (frames > (1u << 24)) return fail(err, "WavPack block too large");
    int rate = 0;
    uint32_t ri = (flags & kRateMask) >> kRateLsb;
    if (ri < 15) rate = kRates[ri];
    Block blk;
    if (!decodeBlock(b, flags, frames, crc, 8 + ck, blk, rate, err)) return false;
    if (!out.sampleRate) out.sampleRate = rate > 0 ? rate : 44100;
    out.bitsPerSample = std::max(out.bitsPerSample, blk.bits);
    if (flags & kInitial) {
      group.clear();
      groupChannels = 0;
    }
    groupChannels += blk.channels;
    group.push_back(std::move(blk));
    if (!(flags & kFinal)) continue;
    if (!out.channels) out.channels = groupChannels;
    if (groupChannels != out.channels) return fail(err, "WavPack channel layout changes mid-stream");
    size_t n = group[0].frames;
    for (const auto& g : group)
      if (g.frames != n) return fail(err, "inconsistent WavPack block lengths");
    size_t base = out.interleaved.size();
    out.interleaved.resize(base + n * static_cast<size_t>(out.channels));
    for (size_t f = 0; f < n; ++f) {
      size_t c = 0;
      for (const auto& g : group)
        for (int k = 0; k < g.channels; ++k)
          out.interleaved[base + f * static_cast<size_t>(out.channels) + c++] =
              g.samples[f * static_cast<size_t>(g.channels) + static_cast<size_t>(k)];
    }
    group.clear();
    groupChannels = 0;
  }
  if (out.channels == 0 || out.interleaved.empty()) return fail(err, "WavPack file has no audio");
  return true;
}

}  // namespace gl

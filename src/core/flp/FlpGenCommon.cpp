// Gridline — FLP import: helpers shared by the generator decoders.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "FlpGen.h"

namespace gl::flp::gen {

uint32_t Bytes::u32(size_t at) const {
  if (!has(at, 4)) return 0;
  return static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) | (static_cast<uint32_t>(p[at + 2]) << 16) |
         (static_cast<uint32_t>(p[at + 3]) << 24);
}

float Bytes::f32(size_t at) const {
  if (!has(at, 4)) return 0.0f;
  uint32_t u = u32(at);
  float f;
  std::memcpy(&f, &u, 4);
  return std::isfinite(f) ? f : 0.0f;
}

double Bytes::f64(size_t at) const {
  if (!has(at, 8)) return 0.0;
  uint64_t u = u64(at);
  double d;
  std::memcpy(&d, &u, 8);
  return std::isfinite(d) ? d : 0.0;
}

std::string Bytes::pstr(size_t at, size_t* next) const {
  size_t len = u8(at);
  std::string s;
  if (has(at + 1, len)) s.assign(reinterpret_cast<const char*>(p + at + 1), len);
  if (next) *next = at + 1 + len;
  return s;
}

std::string Bytes::utf16(size_t at, size_t chars) const {
  std::string s;
  for (size_t i = 0; i < chars && has(at + i * 2, 2); ++i) {
    uint32_t cp = u16(at + i * 2);
    if (cp == 0) break;
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return s;
}

size_t Bytes::find(const std::string& pat, size_t from) const {
  if (pat.empty() || n < pat.size()) return std::string::npos;
  for (size_t i = from; i + pat.size() <= n; ++i)
    if (std::memcmp(p + i, pat.data(), pat.size()) == 0) return i;
  return std::string::npos;
}

Bytes stateOf(const std::vector<Event>& events) {
  const Event* e = pluginState(events);
  return e ? Bytes(*e) : Bytes();
}

// ------------------------------------------------------------------ inflate
// A small DEFLATE decoder written from RFC 1951: stored, fixed-Huffman and
// dynamic-Huffman blocks; canonical codes decoded with per-length counts.
namespace {

struct BitIn {
  Bytes b;
  size_t pos = 0;
  uint32_t bit = 0, bits = 0;
  bool bad = false;
  uint32_t need(uint32_t n) {
    while (bits < n) {
      if (pos >= b.n) {
        bad = true;
        return 0;
      }
      bit |= static_cast<uint32_t>(b.p[pos++]) << bits;
      bits += 8;
    }
    uint32_t v = bit & ((1u << n) - 1u);
    bit >>= n;
    bits -= n;
    return v;
  }
};

struct Huff {
  uint16_t count[16] = {};
  uint16_t symbol[320] = {};
  // Builds from code lengths; false when over-subscribed.
  bool build(const uint8_t* lengths, int n) {
    for (auto& c : count) c = 0;
    for (int i = 0; i < n; ++i) count[lengths[i]]++;
    count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;
    }
    uint16_t offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    for (int i = 0; i < n; ++i)
      if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
  }
  int decode(BitIn& in) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= static_cast<int>(in.need(1));
      if (in.bad) return -1;
      int c = count[len];
      if (code - c < first) return symbol[index + (code - first)];
      index += c;
      first += c;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitIn& in, std::vector<uint8_t>& out, const Huff& lit, const Huff& dist, size_t maxOut) {
  for (;;) {
    int sym = lit.decode(in);
    if (sym < 0) return false;
    if (sym < 256) {
      if (out.size() >= maxOut) return false;
      out.push_back(static_cast<uint8_t>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      size_t len = kLenBase[sym] + in.need(kLenExtra[sym]);
      int ds = dist.decode(in);
      if (ds < 0 || ds >= 30) return false;
      size_t d = kDistBase[ds] + in.need(kDistExtra[ds]);
      if (in.bad || d > out.size() || out.size() + len > maxOut) return false;
      for (size_t k = 0; k < len; ++k) out.push_back(out[out.size() - d]);
    }
  }
}

}  // namespace

bool inflateZlib(Bytes in, std::vector<uint8_t>& out, size_t maxOut) {
  out.clear();
  if (in.n < 2 || (in.u8(0) & 0x0F) != 8 || ((in.u8(0) << 8) | in.u8(1)) % 31 != 0 || (in.u8(1) & 0x20)) return false;
  BitIn bi;
  bi.b = in.sub(2, in.n - 2);
  for (;;) {
    const uint32_t last = bi.need(1), type = bi.need(2);
    if (bi.bad) return false;
    if (type == 0) {  // stored
      bi.bit = 0, bi.bits = 0;
      if (!bi.b.has(bi.pos, 4)) return false;
      const uint32_t len = bi.b.u16(bi.pos), nlen = bi.b.u16(bi.pos + 2);
      if ((len ^ 0xFFFFu) != nlen || !bi.b.has(bi.pos + 4, len) || out.size() + len > maxOut) return false;
      out.insert(out.end(), bi.b.p + bi.pos + 4, bi.b.p + bi.pos + 4 + len);
      bi.pos += 4 + len;
    } else if (type == 1 || type == 2) {
      Huff lit, dist;
      uint8_t lengths[320] = {};
      if (type == 1) {
        for (int i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
      } else {
        const int nlen = static_cast<int>(bi.need(5)) + 257, ndist = static_cast<int>(bi.need(5)) + 1, ncode = static_cast<int>(bi.need(4)) + 4;
        if (bi.bad || nlen > 286 || ndist > 30) return false;
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t cl[19] = {};
        for (int i = 0; i < ncode; ++i) cl[order[i]] = static_cast<uint8_t>(bi.need(3));
        Huff lencode;
        if (bi.bad || !lencode.build(cl, 19)) return false;
        int i = 0;
        uint8_t all[320] = {};
        while (i < nlen + ndist) {
          int sym = lencode.decode(bi);
          if (sym < 0) return false;
          if (sym < 16) {
            all[i++] = static_cast<uint8_t>(sym);
            continue;
          }
          int rep = 0;
          uint8_t val = 0;
          if (sym == 16) {
            if (i == 0) return false;
            val = all[i - 1];
            rep = 3 + static_cast<int>(bi.need(2));
          } else if (sym == 17) {
            rep = 3 + static_cast<int>(bi.need(3));
          } else {
            rep = 11 + static_cast<int>(bi.need(7));
          }
          if (bi.bad || i + rep > nlen + ndist) return false;
          while (rep--) all[i++] = val;
        }
        for (int k = 0; k < nlen; ++k) lengths[k] = all[k];
        for (int k = 0; k < ndist; ++k) lengths[288 + k] = all[nlen + k];
      }
      if (!lit.build(lengths, 288) || !dist.build(lengths + 288, 30)) return false;
      if (!inflateBlock(bi, out, lit, dist, maxOut)) return false;
    } else {
      return false;
    }
    if (last) return true;
  }
}

ChanEnv parseEnv(Bytes b) {
  ChanEnv e;
  if (!b.has(0, 68)) return e;
  e.enabled = b.i32(4) != 0;
  e.predelay = b.i32(8), e.attack = b.i32(12), e.hold = b.i32(16), e.decay = b.i32(20);
  e.sustain = b.i32(24), e.release = b.i32(28), e.amount = b.i32(32);
  e.lfoPredelay = b.i32(36), e.lfoAttack = b.i32(40), e.lfoAmount = b.i32(44), e.lfoSpeed = b.i32(48), e.lfoShape = b.i32(52);
  e.attackTension = b.i32(56), e.decayTension = b.i32(60), e.releaseTension = b.i32(64);
  return e;
}

namespace {
ChanFilter parseLevels(Bytes b) {
  ChanFilter f;
  if (!b.has(0, 24)) return f;
  f.cutoff = std::clamp(b.i32(12), 0, 256);
  f.reso = std::clamp(b.i32(16), 0, 256);
  f.type = std::clamp(b.i32(20), 0, 7);
  return f;
}
}  // namespace

ChanSettings channelSettings(const std::vector<Event>& events, Bytes fallbackEnv, Bytes fallbackLevels) {
  ChanSettings s;
  int k = 0;
  for (const auto& e : events) {
    if (e.id == 218 && k < EnvTargets && e.size >= 68) s.env[k++] = parseEnv(Bytes(e)), s.haveEnv = true;
    if (e.id == 219 && e.size >= 24) s.filter = parseLevels(Bytes(e)), s.haveFilter = true;
  }
  if (!s.haveEnv && fallbackEnv.has(0, 68 * EnvTargets)) {
    for (int t = 0; t < EnvTargets; ++t) s.env[t] = parseEnv(fallbackEnv.sub(static_cast<size_t>(t) * 68, 68));
    s.haveEnv = true;
  }
  if (!s.haveFilter && fallbackLevels.has(0, 24)) s.filter = parseLevels(fallbackLevels), s.haveFilter = true;
  return s;
}

double envSeconds(int v) {
  // FL's envelope time knobs span 100..65536 on a strongly curved scale.
  // Same cubic 0..10 s curve as the channel importer (FlpChannels.cpp), which
  // also applies the volume envelope to Trio Synth channels after this decoder.
  double x = std::clamp((v - 100) / (65536.0 - 100.0), 0.0, 1.0);
  return std::max(0.001, 10.0 * x * x * x);
}

double lfoHz(int v) {
  double x = std::clamp((v - 200) / (65536.0 - 200.0), 0.0, 1.0);
  return 0.05 * std::pow(400.0, x);
}

double cutoffHz(int v) {
  double x = std::clamp(v / 256.0, 0.0, 1.0);
  return 10.0 * std::pow(2000.0, x);
}

void setp(PluginState& s, int idx, double v) {
  if (idx < 0) return;
  if (static_cast<size_t>(idx) >= s.params.size()) s.params.resize(static_cast<size_t>(idx) + 1, 0.0f);
  s.params[static_cast<size_t>(idx)] = static_cast<float>(v);
}

PluginState base(const std::string& type) { return defaultPluginState(type); }

void ParamTable::install(PluginMapping& m) const {
  auto t = std::make_shared<std::map<int, Row>>(rows);
  setRawRanges(m, raw);
  m.param = [t](int fl) {
    auto it = t->find(fl);
    return it == t->end() ? -1 : it->second.gl;
  };
  m.value = [t](int fl, double n) {
    auto it = t->find(fl);
    if (it == t->end()) return n;
    const Row& r = it->second;
    n = std::clamp(n, 0.0, 1.0);
    if (r.fn) return r.fn(n);
    if (r.expo && r.lo > 0 && r.hi > 0) return r.lo * std::pow(r.hi / r.lo, n);
    return r.lo + n * (r.hi - r.lo);
  };
}

void note(Context& c, const std::string& flName, const std::string& what) { c.report.warn(flName + ": " + what); }

}  // namespace gl::flp::gen

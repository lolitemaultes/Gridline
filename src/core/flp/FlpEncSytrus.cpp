// Gridline — FLP export: Sytrus state writer (J-34 fields, J-39 full writer).
//
// Layout (FlpGenSytrus.cpp): dword header (version), zlib stream of the
// patch. The patch is read into a structure that keeps every stored value
// (Patch, lossless: parse + serialize reproduce each of FL's shipped states
// byte for byte) and Gridline's values are put into it:
//   * template = the state the instance was imported with, or a fresh
//     version-25 patch (defaultPatch) whose values Gridline does not model are
//     the ones FL's shipped version-25 states hold most often (format
//     constants: articulator headers, neutral mapping curves, option words);
//   * apply() writes every value the decoder reads — parameters through the
//     inverse of the decoder's laws, the drawn data (harmonic levels / phases,
//     envelope / LFO / mapping / waveshaper curves) from PluginState::data —
//     and leaves values that already decode to Gridline's untouched, so an
//     imported state keeps its other bytes;
//   * the result is stored uncompressed (stored deflate blocks) so the J-34
//     fields (fixed offsets) can solve what the inverse laws missed through
//     the decoder itself, then compressed again (own fixed-Huffman deflate).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "FlpEncode.h"
#include "FlpGen.h"
#include "../PluginInfo.h"
#include "../../packs/synths/matrixfm.h"
#include "../../packs/synths/matrixfm_data.h"

namespace gl::flp::enc {

namespace {

namespace mf = gl::packs::synths::mfm;
constexpr size_t kBlock = 65535;

float pv(const PluginState& s, int k) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : std::numeric_limits<float>::quiet_NaN();
}
int32_t geti(const Bytes& b, size_t at) { return static_cast<int32_t>(getU32(b, at)); }

bool inflate(const Bytes& b, Bytes& raw) {
  if (b.size() < 6) return false;
  return gen::inflateZlib(gen::Bytes(b.data() + 4, b.size() - 4), raw, 4u << 20);
}
uint32_t adler32(const Bytes& d) {
  uint32_t a = 1, s = 0;
  for (uint8_t x : d) a = (a + x) % 65521u, s = (s + a) % 65521u;
  return (s << 16) | a;
}
void putAdler(ByteWriter& w, const Bytes& raw) {
  const uint32_t ad = adler32(raw);
  for (int k = 3; k >= 0; --k) w.u8((ad >> (8 * k)) & 0xFF);
}
// State offset of raw byte o in the stored form.
size_t stored(size_t o) { return 4 + 2 + (o / kBlock + 1) * 5 + o; }
bool isStored(const Bytes& b, size_t rawSize) {
  if (b.size() != 4 + 2 + ((rawSize + kBlock - 1) / kBlock) * 5 + rawSize + 4 || b[4] != 0x78 || b[5] != 0x01) return false;
  for (size_t at = 6, left = rawSize; left;) {
    const size_t n = std::min(left, kBlock);
    if ((b[at] & 6) != 0 || getU16(b, at + 1) != n) return false;
    at += 5 + n, left -= n;
  }
  return true;
}
Bytes storedForm(uint32_t head, const Bytes& raw) {
  ByteWriter w;
  w.u32(head);
  w.u8(0x78), w.u8(0x01);
  for (size_t at = 0; at < raw.size(); at += kBlock) {
    const size_t n = std::min(kBlock, raw.size() - at);
    w.u8(at + n >= raw.size() ? 1 : 0);
    w.u16(static_cast<uint32_t>(n)), w.u16(static_cast<uint32_t>(~n & 0xFFFF));
    w.bytes(raw.data() + at, n);
  }
  putAdler(w, raw);
  return w.b;
}

// Deflate, fixed Huffman codes, greedy LZ77 over a 32 KB window.
struct BitOut {
  Bytes b;
  uint32_t acc = 0, n = 0;
  void bits(uint32_t v, uint32_t k) {  // LSB first
    acc |= v << n, n += k;
    while (n >= 8) b.push_back(static_cast<uint8_t>(acc)), acc >>= 8, n -= 8;
  }
  void huff(uint32_t code, uint32_t len) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (uint32_t i = 0; i < len; ++i) r = (r << 1) | ((code >> i) & 1);
    bits(r, len);
  }
  void flush() {
    if (n) b.push_back(static_cast<uint8_t>(acc));
    acc = 0, n = 0;
  }
};
void litLen(BitOut& o, int sym) {
  if (sym < 144) o.huff(0x30 + static_cast<uint32_t>(sym), 8);
  else if (sym < 256) o.huff(0x190 + static_cast<uint32_t>(sym - 144), 9);
  else if (sym < 280) o.huff(static_cast<uint32_t>(sym - 256), 7);
  else o.huff(0xC0 + static_cast<uint32_t>(sym - 280), 8);
}
Bytes deflateFixed(const Bytes& in) {
  static const uint16_t lenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t lenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t distBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  BitOut o;
  o.bits(1, 1), o.bits(1, 2);  // final block, fixed codes
  constexpr size_t kHash = 1 << 15, kWin = 32768;
  std::vector<int64_t> head(kHash, -1), prev(in.size(), -1);
  auto hash = [&](size_t i) { return ((static_cast<uint32_t>(in[i]) << 10) ^ (static_cast<uint32_t>(in[i + 1]) << 5) ^ in[i + 2]) & (kHash - 1); };
  auto insert = [&](size_t i) {
    if (i + 2 >= in.size()) return;
    const uint32_t h = hash(i);
    prev[i] = head[h], head[h] = static_cast<int64_t>(i);
  };
  for (size_t i = 0; i < in.size();) {
    size_t bestLen = 0, bestDist = 0;
    if (i + 2 < in.size()) {
      int chain = 64;
      for (int64_t j = head[hash(i)]; j >= 0 && i - static_cast<size_t>(j) <= kWin && chain--; j = prev[static_cast<size_t>(j)]) {
        size_t l = 0;
        while (l < 258 && i + l < in.size() && in[static_cast<size_t>(j) + l] == in[i + l]) ++l;
        if (l > bestLen) bestLen = l, bestDist = i - static_cast<size_t>(j);
        if (l == 258) break;
      }
    }
    if (bestLen >= 3) {
      int c = 28;
      while (lenBase[c] > bestLen) --c;
      litLen(o, 257 + c);
      o.bits(static_cast<uint32_t>(bestLen - lenBase[c]), lenExtra[c]);
      int d = 29;
      while (distBase[d] > bestDist) --d;
      o.huff(static_cast<uint32_t>(d), 5);
      o.bits(static_cast<uint32_t>(bestDist - distBase[d]), distExtra[d]);
      for (size_t k = 0; k < bestLen; ++k) insert(i + k);
      i += bestLen;
    } else {
      litLen(o, in[i]);
      insert(i);
      ++i;
    }
  }
  litLen(o, 256);
  o.flush();
  return o.b;
}
Bytes compressed(uint32_t head, const Bytes& raw) {
  ByteWriter w;
  w.u32(head);
  w.u8(0x78), w.u8(0x01);
  w.bytes(deflateFixed(raw));
  putAdler(w, raw);
  return w.b;
}

int versionOf(uint32_t hdr) { return static_cast<int>(hdr > 0xffff ? hdr >> 16 : hdr); }

// ------------------------------------------------------------ the patch structure

struct RPt {
  double x = 0, y = 0;
  float t = 0;
  uint8_t tail[4] = {0, 0, 0, 0};
};
struct List {
  int32_t h = 3, fmt = 3;
  std::vector<RPt> pts;
};
struct Art {
  int32_t h0 = 3;
  uint8_t hdr[16] = {};
  List env;  // h unused (no leading dword)
  int32_t envTail[4] = {0, 0, 1, -1};
  int32_t p1[5] = {2, 128, 128, 0, 128};
  List lfo;
  int32_t lfoTail[4] = {0, 0, -1, -1};
  int32_t p2[6] = {2, 40000, 0, 0, 0, 0};
  List maps[mf::kMaps];
  int32_t mapTail[mf::kMaps][5] = {};
};
struct Unit {
  int32_t u[10] = {};
  int32_t ratio = 20000, freq = 0, noise = 0;
  std::vector<float> lv, ph;
  int32_t tail[5] = {0, 0, 0, 0, 128};
  uint8_t flags[8] = {};
  std::vector<int32_t> fb;
  uint8_t fb9 = 0;
  List ws;
  int32_t wsTail[5] = {0, -1, -1, -1, -1};
  uint8_t wsBip = 0;
  std::vector<Art> arts;
};
struct Patch {
  uint32_t head = 0x00190003;
  int ver = 25;
  int32_t main[240] = {};
  uint8_t m2[2] = {};
  int32_t soften = 0;
  uint8_t opt0 = 0, opt1 = 0, gibbs = 0;
  Bytes b507;
  uint8_t delays[3][23] = {};
  int32_t rev[9] = {};
  uint8_t revOn = 0, revTempo = 0;
  int32_t eqT = 0;
  int32_t eqb[3][4] = {};
  Bytes info;
  Unit units[mf::kUnits];
  Bytes trail;
};

struct Rd {
  const Bytes& b;
  size_t at = 0;
  bool bad = false;
  bool need(size_t k) {
    if (bad || at > b.size() || k > b.size() - at) bad = true;
    return !bad;
  }
  int32_t i32() {
    if (!need(4)) return 0;
    const int32_t v = geti(b, at);
    at += 4;
    return v;
  }
  uint8_t u8() { return need(1) ? b[at++] : 0; }
  float f32() {
    if (!need(4)) return 0;
    const float v = getF32(b, at);
    at += 4;
    return v;
  }
  double f64() {
    if (!need(8)) return 0;
    const double v = getF64(b, at);
    at += 8;
    return v;
  }
  Bytes raw(size_t n) {
    if (!need(n)) return {};
    Bytes v(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    return v;
  }
  void into(uint8_t* p, size_t n) {
    if (need(n)) std::memcpy(p, b.data() + at, n), at += n;
  }
};

void readPts(Rd& r, int fmt, int n, std::vector<RPt>& out) {
  if (n < 0 || n > 100000) return void(r.bad = true);
  out.clear();
  for (int i = 0; i < n && !r.bad; ++i) {
    RPt p;
    if (fmt <= 1) {
      p.x = r.f32(), p.y = r.f32(), p.t = r.f32();
      if (fmt == 1) r.into(p.tail, 4);
    } else {
      p.x = r.f64(), p.y = r.f64(), p.t = r.f32();
      r.into(p.tail, 4);
    }
    out.push_back(p);
  }
}
void writePts(ByteWriter& w, int fmt, const std::vector<RPt>& v) {
  for (const RPt& p : v) {
    if (fmt <= 1) {
      w.f32(static_cast<float>(p.x)), w.f32(static_cast<float>(p.y)), w.f32(p.t);
      if (fmt == 1) w.bytes(p.tail, 4);
    } else {
      w.f64(p.x), w.f64(p.y), w.f32(p.t), w.bytes(p.tail, 4);
    }
  }
}
void readList(Rd& r, List& l) {
  l.h = r.i32(), l.fmt = r.i32();
  const int n = r.i32();
  readPts(r, l.fmt, n, l.pts);
}
void writeList(ByteWriter& w, const List& l) {
  w.i32(l.h), w.i32(l.fmt), w.i32(static_cast<int32_t>(l.pts.size()));
  writePts(w, l.fmt, l.pts);
}

bool parse(const Bytes& raw, uint32_t head, Patch& s) {
  Rd r{raw};
  const int ver = versionOf(head);
  s.head = head, s.ver = ver;
  for (int32_t& v : s.main) v = r.i32();
  r.into(s.m2, 2);
  if (ver > 3) s.soften = r.i32();
  if (ver > 5) s.opt0 = r.u8();
  if (ver > 6) s.opt1 = r.u8();
  if (ver > 12) s.gibbs = r.u8();
  int n507 = 0;
  if (ver > 13) n507 = (ver > 14 ? 5 : 1) + (ver > 17 ? 3 : 0) + (ver > 18 ? 1 : 0);
  s.b507 = r.raw(static_cast<size_t>(n507));
  for (auto& d : s.delays) r.into(d, 23);
  if (ver >= 10) {
    for (int32_t& v : s.rev) v = r.i32();
    s.revOn = r.u8();
    if (ver > 10) s.revTempo = r.u8();
  }
  if (ver > 2) {
    s.eqT = r.i32();
    for (auto& e : s.eqb)
      for (int i = 0; i < (ver > 13 ? 4 : 3); ++i) e[i] = r.i32();
  }
  if (ver < 0x17) {
    const int32_t n = r.i32();
    if (n < 0) return false;
    s.info = r.raw(static_cast<size_t>(n));
  } else {
    int32_t n = r.u8();
    if (n == 0xff) n = r.i32();
    if (n < 0) return false;
    s.info = r.raw(static_cast<size_t>(n) * 2);
  }
  for (int u = 0; u < mf::kUnits && !r.bad; ++u) {
    Unit& U = s.units[u];
    for (int32_t& v : U.u) v = r.i32();
    int na = 2;
    if (u < mf::kOps) {
      na = 6;
      U.ratio = r.i32(), U.freq = r.i32();
      int nh = 24;
      if (ver >= 8) U.noise = r.i32(), nh = ver < 0x11 ? 128 : 256;
      U.lv.resize(static_cast<size_t>(nh)), U.ph.resize(static_cast<size_t>(nh));
      for (float& v : U.lv) v = r.f32();
      for (float& v : U.ph) v = r.f32();
      for (int32_t& v : U.tail) v = r.i32();
      if (ver < 8) {
        U.noise = r.i32();
        r.into(U.flags, 7);
      } else {
        r.into(U.flags, 8);
      }
    } else if (u < mf::kOps + mf::kFilters) {
      na = 8;
      U.fb.resize(ver < 9 ? 10 : 12);
      for (int32_t& v : U.fb) v = r.i32();
      if (ver < 9) U.fb9 = r.u8();
      readList(r, U.ws);
      for (int32_t& v : U.wsTail) v = r.i32();
      if (ver >= 12) U.wsBip = r.u8();
    }
    U.arts.resize(static_cast<size_t>(na));
    for (Art& A : U.arts) {
      A.h0 = r.i32();
      r.into(A.hdr, 16);
      A.env.h = 0;
      A.env.fmt = r.i32();
      const int n = r.i32();
      readPts(r, A.env.fmt, n, A.env.pts);
      for (int32_t& v : A.envTail) v = r.i32();
      for (int32_t& v : A.p1) v = r.i32();
      readList(r, A.lfo);
      for (int32_t& v : A.lfoTail) v = r.i32();
      for (int i = 0; i < (A.h0 >= 3 ? 6 : 5); ++i) A.p2[i] = r.i32();
      for (int k = 0; k < mf::kMaps; ++k) {
        readList(r, A.maps[k]);
        for (int32_t& v : A.mapTail[k]) v = r.i32();
      }
      if (r.bad) break;
    }
  }
  s.trail = r.raw(static_cast<size_t>((ver > 0x17) + (ver > 0x18)));
  return !r.bad && r.at == raw.size();
}

Bytes serialize(const Patch& s) {
  ByteWriter w;
  const int ver = s.ver;
  for (int32_t v : s.main) w.i32(v);
  w.bytes(s.m2, 2);
  if (ver > 3) w.i32(s.soften);
  if (ver > 5) w.u8(s.opt0);
  if (ver > 6) w.u8(s.opt1);
  if (ver > 12) w.u8(s.gibbs);
  w.bytes(s.b507);
  for (const auto& d : s.delays) w.bytes(d, 23);
  if (ver >= 10) {
    for (int32_t v : s.rev) w.i32(v);
    w.u8(s.revOn);
    if (ver > 10) w.u8(s.revTempo);
  }
  if (ver > 2) {
    w.i32(s.eqT);
    for (const auto& e : s.eqb)
      for (int i = 0; i < (ver > 13 ? 4 : 3); ++i) w.i32(e[i]);
  }
  if (ver < 0x17) {
    w.i32(static_cast<int32_t>(s.info.size()));
    w.bytes(s.info);
  } else {
    const size_t n = s.info.size() / 2;
    if (n < 0xff) w.u8(static_cast<uint32_t>(n));
    else w.u8(0xff), w.i32(static_cast<int32_t>(n));
    w.bytes(s.info.data(), n * 2);
  }
  for (int u = 0; u < mf::kUnits; ++u) {
    const Unit& U = s.units[u];
    for (int32_t v : U.u) w.i32(v);
    if (u < mf::kOps) {
      w.i32(U.ratio), w.i32(U.freq);
      if (ver >= 8) w.i32(U.noise);
      for (float v : U.lv) w.f32(v);
      for (float v : U.ph) w.f32(v);
      for (int32_t v : U.tail) w.i32(v);
      if (ver < 8) {
        w.i32(U.noise);
        w.bytes(U.flags, 7);
      } else {
        w.bytes(U.flags, 8);
      }
    } else if (u < mf::kOps + mf::kFilters) {
      for (int32_t v : U.fb) w.i32(v);
      if (ver < 9) w.u8(U.fb9);
      writeList(w, U.ws);
      for (int32_t v : U.wsTail) w.i32(v);
      if (ver >= 12) w.u8(U.wsBip);
    }
    for (const Art& A : U.arts) {
      w.i32(A.h0);
      w.bytes(A.hdr, 16);
      w.i32(A.env.fmt), w.i32(static_cast<int32_t>(A.env.pts.size()));
      writePts(w, A.env.fmt, A.env.pts);
      for (int32_t v : A.envTail) w.i32(v);
      for (int32_t v : A.p1) w.i32(v);
      writeList(w, A.lfo);
      for (int32_t v : A.lfoTail) w.i32(v);
      for (int i = 0; i < (A.h0 >= 3 ? 6 : 5); ++i) w.i32(A.p2[i]);
      for (int k = 0; k < mf::kMaps; ++k) {
        writeList(w, A.maps[k]);
        for (int32_t v : A.mapTail[k]) w.i32(v);
      }
    }
  }
  w.bytes(s.trail);
  return w.b;
}

// ------------------------------------------------------------ fresh patch

RPt pt(double x, double y, float t, bool first) {
  RPt p;
  p.x = x, p.y = y, p.t = t;
  p.tail[3] = first ? 0 : t < 0 ? 255 : t > 0 ? 1 : 2;  // FL: tension sign of every point after the first (2 = straight)
  return p;
}
// FL's neutral mapping curve of articulator a of unit u (what a fresh patch holds).
List neutralMap(int u, int a, int k) {
  List l;
  const double n = mf::artBipolar(u, a) ? 0.5 : 1.0;
  if (u < mf::kOps && a == mf::OaDamp && k == mf::MapKey) {  // operator damping: key ramp
    l.pts = {pt(0, 0.0625, 0, true), pt(1, 0.9375, 0.125f, false)};
  } else if (k == mf::MapUni) {
    l.pts = {pt(0, n, 0, true), pt(0.5, n, 0, false), pt(0.5, n, 0, false)};
  } else {
    l.pts = {pt(0, n, 0, true), pt(1, n, 0, false)};
  }
  return l;
}

std::vector<RPt> fromCurve(const mf::Curve& c);

Patch defaultPatch() {
  Patch s;
  s.head = 0x00190003, s.ver = 25;
  // Main block: unity levels / scales, unison, chorus; option words 188..199.
  const int32_t m0[] = {128, 128, 0};
  std::copy(std::begin(m0), std::end(m0), s.main);
  const int32_t m165[] = {128, 128, 0, 128, 128, 128, 0, 128, 1, 128, 100, 128, 0, 16, 0, 0, 0, 512, 768, 8, 128, -32, 128,
                          128, -1, 256, 256, 1064570999, 0, 1, 0, 3, 16777473, 0, 1};
  std::copy(std::begin(m165), std::end(m165), s.main + 165);
  s.b507 = Bytes{0, 0, 1, 0, 0, 1, 0, 1, 1};
  for (auto& d : s.delays) {
    Bytes b(23, 0);
    putI32(b, 0, 64), putI32(b, 4, 144), putI32(b, 12, 64);
    b[20] = 1;
    std::memcpy(d, b.data(), 23);
  }
  const int32_t rv[9] = {75, 40, 0, 40, 100, 15, 40, 2, 25};
  std::copy(std::begin(rv), std::end(rv), s.rev);
  const int32_t eq[3][4] = {{0, 5777, 17500, 0}, {0, 33145, 17500, 1}, {0, 55825, 17500, 2}};
  std::memcpy(s.eqb, eq, sizeof eq);
  for (int u = 0; u < mf::kUnits; ++u) {
    Unit& U = s.units[u];
    if (u < mf::kOps) {
      U.u[1] = 256, U.u[2] = 1200;
      U.lv.assign(mf::kHarmonics, 0.f), U.ph.assign(mf::kHarmonics, 0.f);
      U.lv[0] = 1.f;
    } else if (u < mf::kOps + mf::kFilters) {
      const int32_t f[10] = {0, 128, 128, 0, 128, 0, 0, 128, 128, 0};
      std::copy(std::begin(f), std::end(f), U.u);
      U.fb = {0, 1056964608, 0, 0, 1056964608, 0, 1065353216, 0, 0, 0, 0, 1};
      U.ws.pts = {pt(0, 0, 0, true), pt(1, 1, 0, false)};
    }
    U.arts.resize(static_cast<size_t>(mf::unitArts(u)));
    for (int a = 0; a < mf::unitArts(u); ++a) {
      Art& A = U.arts[static_cast<size_t>(a)];
      const uint8_t hdr[16] = {0, 0, 0, 0, 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f, 1, 0, 0, 0};
      std::memcpy(A.hdr, hdr, 16);
      A.env.h = 0;
      A.lfo.h = 2;
      // Matrix FM's default curves, so a fresh articulator has points (FL reads
      // none as an empty envelope) and decodes to the defaults.
      const mf::Curve env = mf::defaultEnvelope(), lfo = mf::defaultLfo();
      A.env.pts = fromCurve(env);
      A.p1[0] = env.sustain, A.envTail[2] = env.decay;
      A.lfo.pts = fromCurve(lfo);
      A.p2[0] = static_cast<int32_t>(lfo.pts.size()) - 2;
      for (int k = 0; k < mf::kMaps; ++k) A.maps[k] = neutralMap(u, a, k), A.mapTail[k][1] = A.mapTail[k][2] = A.mapTail[k][3] = A.mapTail[k][4] = -1;
    }
  }
  s.trail = Bytes{1, 0};
  return s;
}

// ------------------------------------------------------------ Gridline values → patch

double clampd(double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }
int32_t rnd(double v) { return static_cast<int32_t>(std::lround(std::clamp(v, -2147483648.0, 2147483647.0))); }

// Sets `raw` so that law(raw) == want (inverse guess, then neighbours); a raw
// value that already decodes to `want` is kept.
template <typename Law, typename Inv>
void fit(int32_t& raw, double want, Law law, Inv inv) {
  if (!std::isfinite(want) || static_cast<float>(law(raw)) == static_cast<float>(want)) return;
  const int32_t g = rnd(inv(want));
  int32_t best = g;
  double be = std::fabs(law(g) - want);
  for (int d = -4; d <= 4 && be > 0; ++d) {
    const double e = std::fabs(law(g + d) - want);
    if (e < be) be = e, best = g + d;
  }
  raw = best;
}
void fitScale(int32_t& raw, double want, double div, double lo, double hi) {
  fit(raw, want, [=](int32_t r) { return clampd(r / div, lo, hi); }, [=](double v) { return v * div; });
}
void fitByte(uint8_t& b, float want) {
  if (std::isfinite(want) && (b != 0) != (want >= 0.5f)) b = want >= 0.5f ? 1 : 0;
}

// Curves as the decoder reads them (FlpGenSytrus.cpp toCurve: tension sign flipped).
mf::Curve toCurve(const List& l) {
  mf::Curve c;
  for (const RPt& p : l.pts)
    c.pts.push_back({static_cast<float>(clampd(p.x, 0, 1000)), static_cast<float>(clampd(p.y, -1, 2)), static_cast<float>(-clampd(p.t, -1, 1))});
  return c;
}
bool samePts(const mf::Curve& a, const mf::Curve& b) {
  if (a.pts.size() != b.pts.size()) return false;
  for (size_t i = 0; i < a.pts.size(); ++i)
    if (a.pts[i].dt != b.pts[i].dt || a.pts[i].y != b.pts[i].y || a.pts[i].tension != b.pts[i].tension) return false;
  return true;
}
std::vector<RPt> fromCurve(const mf::Curve& c) {
  std::vector<RPt> v;
  for (size_t i = 0; i < c.pts.size(); ++i) v.push_back(pt(c.pts[i].dt, c.pts[i].y, -c.pts[i].tension, i == 0));
  return v;
}
bool flatAt(const List& l, double y) {
  for (const RPt& p : l.pts)
    if (std::fabs(p.y - y) > 1e-4) return false;
  return true;
}
// The mapping curve Gridline reads from a list (empty = neutral), as FlpGenSytrus.cpp.
mf::Curve mapOf(const List& L, int u, int a, int k) {
  const bool bip = mf::artBipolar(u, a);
  if (L.pts.empty() || flatAt(L, bip ? 0.5 : 1.0)) return {};
  if (u < mf::kOps && a == mf::OaDamp && k == mf::MapKey && L.pts.size() == 2 && std::fabs(L.pts[0].y - 0.0625) < 0.002 &&
      std::fabs(L.pts[1].y - 0.9375) < 0.002)
    return {};
  return toCurve(L);
}

void apply(const PluginState& g, Patch& s) {
  using namespace mf;
  auto P = [&](int k) { return static_cast<double>(pv(g, k)); };
  int32_t* M = s.main;
  const int ver = s.ver;
  fitScale(M[0], P(MVolume), 128, -1e9, 1e9);
  fitScale(M[1], P(MLfoLevel), 128, -1, 1);
  fit(M[2], P(MPitch), [](int32_t r) { return clampd(r, -24, 24); }, [](double v) { return v; });
  // Matrix: a cell Gridline uses must not be muted (bytes 800.. of the main block).
  auto muteByte = [&](int layer, int r, int col) -> uint8_t* {
    return reinterpret_cast<uint8_t*>(M) + 800 + layer * 81 + r * 9 + col;
  };
  for (int r = 0; r < kRows; ++r)
    for (int col = 0; col < kCols; ++col)
      for (int layer = 0; layer < 2; ++layer) {
        if (layer == 1 && (r >= kOps || col >= kOps)) continue;
        const double want = P(layer ? rm(r, col) : fm(r, col));
        int32_t& cell = M[(layer ? 84 : 3) + r * 9 + col];
        const bool mutable_ = ver >= 2;
        uint8_t* mb = muteByte(layer, r, col);
        const double have = mutable_ && *mb ? 0.0 : clampd(cell / 256.0, -1, 1);
        if (static_cast<float>(have) == static_cast<float>(want)) continue;
        if (mutable_) *mb = 0;
        int32_t c = cell;
        fitScale(c, want, 256, -1, 1);
        cell = c;
      }
  const int adsr[8] = {MVolA, MVolD, MVolS, MVolR, MFltA, MFltD, MFltS, MFltR};
  for (int i = 0; i < 8; ++i) fitScale(M[165 + i], P(adsr[i]), 128, i % 4 == 2 ? -1 : 0, i % 4 == 2 ? 1 : 2);
  fit(M[173], P(MUniOrder), [](int32_t r) { return clampd(r, 1, 9); }, [](double v) { return v; });
  fitScale(M[174], P(MUniPan), 128, 0, 1);
  fitScale(M[175], P(MUniVol), 128, 0.125, 1);
  fitScale(M[176], P(MUniPitch), 256, 0, 1);
  fitScale(M[177], P(MUniSub), 128, 0, 0.75);
  fitScale(M[178], P(MUniPhase), 128, 0, 1);
  fitScale(M[179], P(MUniEnv), 128, 0, 1.5);
  fitScale(M[180], P(FxPan), 128, -1, 1);
  fit(M[181], P(ChOrder), [](int32_t r) { return clampd(r, 0, 9); }, [](double v) { return v; });
  fitScale(M[182], P(ChDepth), 1024, 0, 1);
  fitScale(M[183], P(ChSpeed), 1024, 0, 1);
  fitScale(M[184], P(ChDelay), 128, 0, 1);
  fitScale(M[185], P(ChSpread), 128, 0, 1);
  fitScale(M[186], P(ChCross), 128, -1, 1);
  fitScale(M[187], P(ChVol), 128, -1, 1);
  fitScale(M[190], P(MModX), 512, 0, 1);
  fitScale(M[191], P(MModY), 512, 0, 1);
  if (ver > 3) fitScale(s.soften, P(MSoften), 128, 0, 1);
  if (ver > 12) fitByte(s.gibbs, pv(g, MGibbs));
  // Delays: feedback, time, offset, volume, mode (dwords), tempo / ? / on bytes.
  for (int i = 0; i < 3; ++i) {
    Bytes d(s.delays[i], s.delays[i] + 23);
    int32_t fb = geti(d, 0), time = geti(d, 4), ofs = geti(d, 8), vol = geti(d, 12), mode = geti(d, 16);
    uint8_t tempo = d[20], on = d[22];
    fitByte(on, pv(g, dly(i, 0)));
    fit(mode, P(dly(i, 1)), [](int32_t r) { return clampd(r, 0, 2); }, [](double v) { return v; });
    fitScale(fb, P(dly(i, 2)), 128, 0, 0.99);
    fitScale(time, P(dly(i, 3)), 192, 0.01, 4);
    fitScale(ofs, P(dly(i, 4)), 512, -1, 1);
    fitScale(vol, P(dly(i, 5)), 128, -1, 1);
    fitByte(tempo, pv(g, dly(i, 6)));
    putI32(d, 0, fb), putI32(d, 4, time), putI32(d, 8, ofs), putI32(d, 12, vol), putI32(d, 16, mode);
    d[20] = tempo, d[22] = on;
    std::memcpy(s.delays[i], d.data(), 23);
  }
  if (ver >= 10) {
    if (ver > 10) fitByte(s.revTempo, pv(g, RvTempo));
    fitByte(s.revOn, pv(g, RvOn));
    fit(s.rev[0], P(RvLowCut), [](int32_t r) { return clampd(r, 19, 3000); }, [](double v) { return v; });
    fitScale(s.rev[1], P(RvHighCut), 0.01, 500, 22000);
    fitScale(s.rev[2], P(RvPreDelay), s.revTempo ? 192.0 : 384.0, 0, 0.5);
    fitScale(s.rev[3], P(RvRoom), 100, 0.01, 1);
    fitScale(s.rev[4], P(RvDiff), 100, 0, 1);
    fitScale(s.rev[5], P(RvDecay), 10, 0.1, 20);
    fitScale(s.rev[6], P(RvHighDamp), 0.01, 500, 22000);
    fit(s.rev[7], P(RvColor), [](int32_t r) { return clampd(r, 0, 4); }, [](double v) { return v; });
    fitScale(s.rev[8], P(RvVol), 128, -1, 1);
  }
  if (ver > 2) {
    fit(s.eqT, P(MEqTarget), [](int32_t r) { return clampd(r, 0, 3); }, [](double v) { return v; });
    for (int i = 0; i < 3; ++i) {
      int32_t* e = s.eqb[i];
      if (ver > 13) fit(e[3], P(eq(i, 0)), [](int32_t r) { return clampd(r, 0, 2); }, [](double v) { return v; });
      fit(e[1], P(eq(i, 1)), [](int32_t r) { return clampd(20.0 * std::pow(1000.0, r / 65536.0), 20, 20000); },
          [](double v) { return std::log(v / 20.0) / std::log(1000.0) * 65536.0; });
      fitScale(e[2], P(eq(i, 2)), 65536.0 / 4.0, 0.1, 4);
      fitScale(e[0], P(eq(i, 3)), 100, -18, 18);
    }
  }
  // Units.
  PatchData want;
  if (!parsePatch(g.data, want)) want = PatchData{};
  // Harmonic phases just below 1 print as "1" in the data text, which parses
  // back as 0 (phases wrap): those are the largest float below 1.
  {
    size_t pos = 0;
    while ((pos = g.data.find("\nh ", pos)) != std::string::npos) {
      ++pos;
      const size_t end = g.data.find('\n', pos);
      const std::string line = g.data.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      int u = -1;
      char* q = nullptr;
      u = static_cast<int>(std::strtol(line.c_str() + 2, &q, 10));
      if (u < 0 || u >= kOps || !q) continue;
      while (*q == ' ') {
        ++q;
        char* e = nullptr;
        const long h = std::strtol(q, &e, 10);
        if (e == q || *e != ':') break;
        q = e + 1;
        std::strtod(q, &e);  // level
        if (*e != ':') break;
        q = e + 1;
        const double ph = std::strtod(q, &e);
        if (e == q) break;
        q = e;
        if (h >= 0 && h < kHarmonics && ph >= 1.0) want.phase[static_cast<size_t>(u)][static_cast<size_t>(h)] = std::nextafter(1.0f, 0.0f);
      }
    }
  }
  for (int u = 0; u < kUnits; ++u) {
    Unit& U = s.units[u];
    if (u < kOps) {
      fitScale(U.u[0], P(op(u, OpPhase)), 256, 0, 1);
      fitScale(U.u[1], P(op(u, OpVol)), 256, -1, 1);
      fitScale(U.u[2], P(op(u, OpPitchRange)), 100, 0, 48);
      fitScale(U.ratio, P(op(u, OpRatio)), 20000, 0, 32);
      fitScale(U.freq, P(op(u, OpFreq)), 100, -5000, 5000);
      fitScale(U.noise, P(op(u, OpNoise)), 128, 0, 1);
      fitScale(U.tail[0], P(op(u, OpShape)), 256, 0, 2);
      fitScale(U.tail[1], P(op(u, OpTension)), 128, -1, 1);
      fitScale(U.tail[2], P(op(u, OpSkew)), 128, 0, 1);
      fitScale(U.tail[3], P(op(u, OpSine)), 128, 0, 1);
      fitScale(U.tail[4], P(op(u, OpFilter)), 128, 0, 1);
      const int flagParams[7] = {OpHalf, OpEven, OpAbs, OpGlobal, OpCenter, OpDeclick, OpPluck};
      for (int i = 0; i < 7; ++i) fitByte(U.flags[i], pv(g, op(u, flagParams[i])));
      // Harmonic levels / phases.
      const auto& lv = want.level[static_cast<size_t>(u)];
      const auto& ph = want.phase[static_cast<size_t>(u)];
      bool same = true;
      for (size_t h = 0; h < lv.size() && same; ++h) {
        const bool in = h < U.lv.size();
        const float l = in ? static_cast<float>(clampd(U.lv[h], 0, 1)) : 0.f;
        const double p0 = in && std::isfinite(U.ph[h]) ? U.ph[h] : 0.0;
        const float p = in ? static_cast<float>(p0 - std::floor(p0)) : 0.f;
        same = l == lv[h] && p == ph[h];
      }
      if (!same)
        for (size_t h = 0; h < U.lv.size(); ++h) U.lv[h] = h < lv.size() ? lv[h] : 0.f, U.ph[h] = h < ph.size() ? ph[h] : 0.f;
    } else if (u < kOps + kFilters) {
      const int j = u - kOps;
      fit(U.u[0], P(flt(j, FlMode)), [](int32_t r) { return clampd(r, 0, kFltModes - 1); }, [](double v) { return v; });
      fitScale(U.u[1], P(flt(j, FlEnv)), 128, 0, 2);
      fitScale(U.u[2], P(flt(j, FlCut)), 256, 0, 1);
      fitScale(U.u[3], P(flt(j, FlRes)), 256, 0, 1);
      fitScale(U.u[4], P(flt(j, FlLow)), 128, 0, 1);
      fitScale(U.u[5], P(flt(j, FlBand)), 128, 0, 1);
      fitScale(U.u[6], P(flt(j, FlHigh)), 128, 0, 1);
      fitScale(U.u[7], P(flt(j, FlWsAmp)), 128, 0.5, 1.5);
      fitScale(U.u[8], P(flt(j, FlWsMix)), 128, 0, 1);
      fitScale(U.u[9], P(flt(j, FlNext)), 256, -1, 1);
      if (U.fb.size() >= 12) {
        fit(U.fb[11], P(flt(j, FlType)), [](int32_t r) { return clampd(r, 0, kFltTypes - 1); }, [](double v) { return v; });
        if ((U.fb[10] != 0) != (pv(g, flt(j, FlWsOn)) >= 0.5f)) U.fb[10] = pv(g, flt(j, FlWsOn)) >= 0.5f ? 1 : 0;
      }
      if (!U.fb.empty()) {
        uint8_t hq = U.fb[0] & 0xff, flat = (U.fb[0] >> 8) & 0xff;
        fitByte(hq, pv(g, flt(j, FlHq)));
        fitByte(flat, pv(g, flt(j, FlFlat)));
        U.fb[0] = static_cast<int32_t>((static_cast<uint32_t>(U.fb[0]) & 0xFFFF0000u) | hq | (static_cast<uint32_t>(flat) << 8));
      }
      if (ver >= 12) fitByte(U.wsBip, pv(g, flt(j, FlWsBipolar)));
      // Waveshaper: straight unless drawn.
      const Curve& ws = want.ws[static_cast<size_t>(j)];
      const bool straightHave = U.ws.pts.size() == 2 && std::fabs(U.ws.pts[0].y) < 1e-4 && std::fabs(U.ws.pts[1].y - 1) < 1e-4;
      const bool haveEmpty = straightHave || U.ws.pts.empty();
      if (ws.empty() ? !haveEmpty : (haveEmpty || !samePts(toCurve(U.ws), ws))) {
        U.ws.fmt = std::max(U.ws.fmt, 2);
        U.ws.pts = ws.empty() ? std::vector<RPt>{pt(0, 0, 0, true), pt(1, 1, 0, false)} : fromCurve(ws);
      }
    }
    for (int a = 0; a < static_cast<int>(U.arts.size()) && a < unitArts(u); ++a) {
      Art& A = U.arts[static_cast<size_t>(a)];
      const ArtData& ad = want.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
      // Envelope curve + sustain (p1[0]) / decay point / loop start / tempo flag.
      {
        const Curve& e = ad.env;
        mf::Curve have;
        if (A.env.pts.size() >= 2) {
          have = toCurve(A.env);
          const int n = static_cast<int>(have.pts.size());
          const int sus = A.p1[0], dec = A.envTail[2], loop = A.envTail[3];
          have.sustain = sus >= 0 && sus < n ? sus : -1;
          have.decay = dec >= 0 && dec < n ? dec : 0;
          have.loopStart = loop >= 0 && loop < have.sustain ? loop : -1;
          have.loopEnd = have.loopStart >= 0 ? have.sustain : -1;
          have.tempo = (A.envTail[0] & 1) != 0;
        } else {
          have = defaultEnvelope();
        }
        const bool same = samePts(have, e) && have.sustain == e.sustain && have.decay == e.decay && have.loopStart == e.loopStart &&
                          have.loopEnd == e.loopEnd && have.tempo == e.tempo;
        // (also when the stored envelope has no points but is to be on: the
        // importer reads "on" only from an envelope with points)
        const bool needPts = A.env.pts.size() < 2 && pv(g, art(u, a, AfEnvOn)) >= 0.5f;
        if ((!same || needPts) && e.pts.size() >= 2) {
          A.env.fmt = std::max(A.env.fmt, 2);
          A.env.pts = fromCurve(e);
          A.p1[0] = e.sustain;
          A.envTail[2] = e.decay;
          A.envTail[3] = e.loopStart >= 0 && e.loopEnd == e.sustain ? e.loopStart : -1;
          A.envTail[0] = (A.envTail[0] & ~1) | (e.tempo ? 1 : 0);
        }
      }
      // LFO curve + tension / pulse width / phase.
      {
        const Curve& l = ad.lfo;
        const mf::Curve have = A.lfo.pts.empty() ? defaultLfo() : toCurve(A.lfo);
        const bool needPts = A.lfo.pts.empty() && pv(g, art(u, a, AfLfoOn)) >= 0.5f;  // (as for the envelope)
        if ((!samePts(have, l) || needPts) && !l.pts.empty()) {
          A.lfo.fmt = std::max(A.lfo.fmt, 2);
          A.lfo.pts = fromCurve(l);
          const int n = static_cast<int>(l.pts.size());
          A.p2[0] = n >= 3 ? n - 2 : -1;
        }
        fit(A.p2[2], ad.lfoTension, [](int32_t r) { return static_cast<float>(clampd(r / 128.0, -1, 1)); }, [](double v) { return v * 128; });
        fit(A.p2[4], ad.lfoPw, [](int32_t r) { return static_cast<float>(clampd(r / 128.0, -1, 1)); }, [](double v) { return v * 128; });
        if (A.h0 >= 3) {
          const double ph = ad.lfoPhase;
          const auto law = [](int32_t r) { return static_cast<float>(static_cast<uint32_t>(r) / 4294967296.0); };
          if (law(A.p2[5]) != static_cast<float>(ph)) {
            const uint32_t g0 = static_cast<uint32_t>(std::llround(std::clamp(ph, 0.0, 1.0) * 4294967296.0) & 0xFFFFFFFFll);
            uint32_t best = g0;
            for (int d = -256; d <= 256; ++d) {
              const uint32_t c = g0 + static_cast<uint32_t>(d);
              if (law(static_cast<int32_t>(c)) == static_cast<float>(ph)) {
                best = c;
                break;
              }
            }
            A.p2[5] = static_cast<int32_t>(best);
          }
        }
      }
      // Switches and scales.
      {
        const bool envOn = pv(g, art(u, a, AfEnvOn)) >= 0.5f;
        if ((A.envTail[1] != 0) != envOn) A.envTail[1] = envOn ? -1 : 0;
        const bool lfoOn = pv(g, art(u, a, AfLfoOn)) >= 0.5f;
        if ((A.lfoTail[1] != 0) != lfoOn) A.lfoTail[1] = lfoOn ? -1 : 0;
        fitScale(A.p1[1], P(art(u, a, AfAtt)), 128, 0, 2);
        fitScale(A.p1[2], P(art(u, a, AfDec)), 128, 0, 2);
        fitScale(A.p1[3], P(art(u, a, AfSus)), 128, -1, 1);
        fitScale(A.p1[4], P(art(u, a, AfRel)), 128, 0, 2);
        const bool tempo = pv(g, art(u, a, AfTempo)) >= 0.5f, global = pv(g, art(u, a, AfLfoGlobal)) >= 0.5f;
        A.lfoTail[0] = (A.lfoTail[0] & ~3) | (tempo ? 1 : 0) | (global ? 2 : 0);
        const double sp = P(art(u, a, AfLfoSpeed));
        fit(A.p2[1], sp,
            [tempo](int32_t r) {
              const double hz = (std::pow(10001.0, std::clamp(r, 0, 65536) / 65536.0) - 1.0) / 625.0;
              return clampd(tempo ? hz * 0.5 : hz, 0.01, 50);
            },
            [tempo](double v) { return std::log((tempo ? v * 2 : v) * 625.0 + 1.0) / std::log(10001.0) * 65536.0; });
        fitScale(A.p2[3], P(art(u, a, AfLfoSkew)), 128, -1, 1);
      }
      // Mapping curves (empty = FL's neutral curve).
      for (int k = 0; k < kMaps; ++k) {
        const Curve& m = ad.maps[static_cast<size_t>(k)];
        const mf::Curve have = mapOf(A.maps[k], u, a, k);
        if (have.empty() && m.empty()) continue;
        if (!have.empty() && !m.empty() && samePts(have, m)) continue;
        const int32_t h = A.maps[k].h;
        A.maps[k] = m.empty() ? neutralMap(u, a, k) : List{h, 3, fromCurve(m)};
        A.maps[k].h = h, A.maps[k].fmt = std::max(A.maps[k].fmt, 2);
      }
    }
  }
}

// ------------------------------------------------------------ encoder parts

// Offsets (in the uncompressed patch) of everything the decoder reads as one number.
struct Offsets {
  size_t soften = 0, gibbs = 0, delays = 0, rev = 0, revOn = 0, revTempo = 0, eq = 0;
  bool hasSoften = false, hasGibbs = false, hasRev = false, hasRevTempo = false, hasEq = false, eqType = false;
  struct Unit {
    size_t u = 0, ratio = 0, freq = 0, noise = 0, tail = 0, flags = 0, fb = 0, wsBip = 0;
    bool hasWsBip = false;
    int nf = 12;
    struct Art {
      size_t envTail = 0, p1 = 0, lfoTail = 0, p2 = 0;
    };
    std::vector<Art> arts;
  } units[mf::kUnits];
  bool ok = false;
};

Offsets offsets(const Bytes& raw, int ver) {
  Offsets o;
  size_t at = 0;
  bool bad = false;
  auto need = [&](size_t k) {
    if (bad || at + k > raw.size()) bad = true;
    return !bad;
  };
  auto i32 = [&]() {
    if (!need(4)) return 0;
    const int v = geti(raw, at);
    at += 4;
    return v;
  };
  auto skip = [&](size_t k) {
    if (need(k)) at += k;
  };
  auto pts = [&](int fmt, int n) {
    if (n < 0 || n > 100000) return void(bad = true);
    skip(static_cast<size_t>(n) * (fmt <= 0 ? 12 : fmt == 1 ? 16 : 24));
  };
  auto list = [&]() {
    i32();
    const int fmt = i32(), n = i32();
    pts(fmt, n);
  };
  skip(240 * 4 + 2);
  if (ver > 3) o.soften = at, o.hasSoften = true, skip(4);
  if (ver > 5) skip(1);
  if (ver > 6) skip(1);
  if (ver > 12) o.gibbs = at, o.hasGibbs = true, skip(1);
  int n507 = 0;
  if (ver > 13) n507 = (ver > 14 ? 5 : 1) + (ver > 17 ? 3 : 0) + (ver > 18 ? 1 : 0);
  skip(static_cast<size_t>(n507));
  o.delays = at, skip(3 * 23);
  if (ver >= 10) {
    o.rev = at, o.hasRev = true, skip(36);
    o.revOn = at, skip(1);
    if (ver > 10) o.revTempo = at, o.hasRevTempo = true, skip(1);
  }
  if (ver > 2) o.eq = at, o.hasEq = true, o.eqType = ver > 13, skip(4 + 3 * (ver > 13 ? 16 : 12));
  if (ver < 0x17) {
    const int len = i32();
    if (len < 0) return o;
    skip(static_cast<size_t>(len));
  } else {
    int len = need(1) ? raw[at++] : 0;
    if (len == 0xff) len = i32();
    if (len < 0) return o;
    skip(static_cast<size_t>(len) * 2);
  }
  for (int u = 0; u < mf::kUnits && !bad; ++u) {
    Offsets::Unit& U = o.units[u];
    U.u = at, skip(40);
    int na = 2;
    if (u < mf::kOps) {
      na = 6;
      int nh = 0;
      U.ratio = at, U.freq = at + 4;
      if (ver < 8) {
        skip(8), nh = 24;
      } else {
        U.noise = at + 8, skip(12), nh = ver < 0x11 ? 128 : 256;
      }
      skip(static_cast<size_t>(nh) * 8);
      U.tail = at, skip(20);
      if (ver < 8) U.noise = at, skip(4), U.flags = at, skip(7);
      else U.flags = at, skip(8);
    } else if (u < mf::kOps + mf::kFilters) {
      na = 8;
      U.nf = ver < 9 ? 10 : 12;
      U.fb = at, skip(static_cast<size_t>(U.nf) * 4);
      if (ver < 9) skip(1);
      list();
      skip(20);
      if (ver >= 12) U.wsBip = at, U.hasWsBip = true, skip(1);
    }
    for (int a = 0; a < na && !bad; ++a) {
      Offsets::Unit::Art A;
      const int h0 = i32();
      skip(16);
      const int fmt = i32(), n = i32();
      pts(fmt, n);
      A.envTail = at, skip(16);
      A.p1 = at, skip(20);
      list();
      A.lfoTail = at, skip(16);
      A.p2 = at, skip(h0 >= 3 ? 24 : 20);
      for (int k = 0; k < mf::kMaps; ++k) list(), skip(20);
      U.arts.push_back(A);
    }
  }
  o.ok = !bad;
  return o;
}

int version(const Bytes& b) { return versionOf(b.size() >= 4 ? getU32(b, 0) : 0); }

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  Bytes raw;
  if (!inflate(b, raw)) return f;
  const Offsets o = offsets(raw, version(b));
  if (!o.ok) return f;
  auto F = [&](int gl, size_t rawAt, int lo, int hi, Type t = Type::I32) { f.push_back(field(gl, stored(rawAt), t, lo, hi)); };
  auto M = [&](int gl, int i, int lo, int hi) { F(gl, static_cast<size_t>(i) * 4, lo, hi); };
  M(mf::MVolume, 0, 0, 256), M(mf::MLfoLevel, 1, -128, 128), M(mf::MPitch, 2, -48, 48);
  for (int r = 0; r < mf::kRows; ++r)
    for (int col = 0; col < mf::kCols; ++col) {
      M(mf::fm(r, col), 3 + r * 9 + col, -256, 256);
      if (r < mf::kOps && col < mf::kOps) M(mf::rm(r, col), 84 + r * 9 + col, -256, 256);
    }
  const int adsr[8] = {mf::MVolA, mf::MVolD, mf::MVolS, mf::MVolR, mf::MFltA, mf::MFltD, mf::MFltS, mf::MFltR};
  for (int i = 0; i < 8; ++i) M(adsr[i], 165 + i, i % 4 == 2 ? -128 : 0, i % 4 == 2 ? 128 : 256);
  M(mf::MUniOrder, 173, 1, 9), M(mf::MUniPan, 174, 0, 128), M(mf::MUniVol, 175, 16, 128), M(mf::MUniPitch, 176, 0, 256);
  M(mf::MUniSub, 177, 0, 96), M(mf::MUniPhase, 178, 0, 128), M(mf::MUniEnv, 179, 0, 192), M(mf::FxPan, 180, -128, 128);
  M(mf::ChOrder, 181, 0, 9), M(mf::ChDepth, 182, 0, 1024), M(mf::ChSpeed, 183, 0, 1024), M(mf::ChDelay, 184, 0, 128);
  M(mf::ChSpread, 185, 0, 128), M(mf::ChCross, 186, -128, 128), M(mf::ChVol, 187, -128, 128);
  M(mf::MModX, 190, 0, 512), M(mf::MModY, 191, 0, 512);
  if (o.hasSoften) F(mf::MSoften, o.soften, 0, 128);
  if (o.hasGibbs) F(mf::MGibbs, o.gibbs, 0, 1, Type::U8);
  for (int i = 0; i < 3; ++i) {
    const size_t d = o.delays + static_cast<size_t>(i) * 23;
    F(mf::dly(i, 0), d + 22, 0, 1, Type::U8), F(mf::dly(i, 1), d + 16, 0, 2), F(mf::dly(i, 2), d, 0, 128);
    F(mf::dly(i, 3), d + 4, 0, 768), F(mf::dly(i, 4), d + 8, -512, 512), F(mf::dly(i, 5), d + 12, -128, 128);
    F(mf::dly(i, 6), d + 20, 0, 1, Type::U8);
  }
  if (o.hasRev) {
    const int rv[9] = {mf::RvLowCut, mf::RvHighCut, mf::RvPreDelay, mf::RvRoom, mf::RvDiff, mf::RvDecay, mf::RvHighDamp, mf::RvColor, mf::RvVol};
    const int lo[9] = {19, 5, 0, 1, 0, 1, 5, 0, -128}, hi[9] = {3000, 220, 192, 100, 100, 200, 220, 4, 128};
    for (int i = 0; i < 9; ++i) F(rv[i], o.rev + static_cast<size_t>(i) * 4, lo[i], hi[i]);
    F(mf::RvOn, o.revOn, 0, 1, Type::U8);
    if (o.hasRevTempo) F(mf::RvTempo, o.revTempo, 0, 1, Type::U8);
  }
  if (o.hasEq) {
    F(mf::MEqTarget, o.eq, 0, 3);
    const size_t stride = o.eqType ? 16 : 12;
    for (int i = 0; i < 3; ++i) {
      const size_t e = o.eq + 4 + static_cast<size_t>(i) * stride;
      F(mf::eq(i, 3), e, -1800, 1800), F(mf::eq(i, 1), e + 4, 0, 65536), F(mf::eq(i, 2), e + 8, 0, 65536);
      if (o.eqType) F(mf::eq(i, 0), e + 12, 0, 2);
    }
  }
  for (int u = 0; u < mf::kUnits; ++u) {
    const Offsets::Unit& U = o.units[u];
    if (u < mf::kOps) {
      F(mf::op(u, mf::OpPhase), U.u, 0, 256), F(mf::op(u, mf::OpVol), U.u + 4, -256, 256), F(mf::op(u, mf::OpPitchRange), U.u + 8, 0, 4800);
      F(mf::op(u, mf::OpRatio), U.ratio, 0, 640000), F(mf::op(u, mf::OpFreq), U.freq, -500000, 500000), F(mf::op(u, mf::OpNoise), U.noise, 0, 128);
      F(mf::op(u, mf::OpShape), U.tail, 0, 512), F(mf::op(u, mf::OpTension), U.tail + 4, -128, 128), F(mf::op(u, mf::OpSkew), U.tail + 8, 0, 128);
      F(mf::op(u, mf::OpSine), U.tail + 12, 0, 128), F(mf::op(u, mf::OpFilter), U.tail + 16, 0, 128);
      const int flagParams[7] = {mf::OpHalf, mf::OpEven, mf::OpAbs, mf::OpGlobal, mf::OpCenter, mf::OpDeclick, mf::OpPluck};
      for (int i = 0; i < 7; ++i) F(mf::op(u, flagParams[i]), U.flags + static_cast<size_t>(i), 0, 1, Type::U8);
    } else if (u < mf::kOps + mf::kFilters) {
      const int j = u - mf::kOps;
      const int fl[10] = {mf::FlMode, mf::FlEnv, mf::FlCut, mf::FlRes, mf::FlLow, mf::FlBand, mf::FlHigh, mf::FlWsAmp, mf::FlWsMix, mf::FlNext};
      const int lo[10] = {0, 0, 0, 0, 0, 0, 0, 64, 0, -256}, hi[10] = {mf::kFltModes - 1, 256, 256, 256, 128, 128, 128, 192, 128, 256};
      for (int i = 0; i < 10; ++i) F(mf::flt(j, fl[i]), U.u + static_cast<size_t>(i) * 4, lo[i], hi[i]);
      if (U.nf == 12) F(mf::flt(j, mf::FlType), U.fb + 44, 0, mf::kFltTypes - 1);
      F(mf::flt(j, mf::FlWsOn), U.fb + 40, 0, 1);
      F(mf::flt(j, mf::FlHq), U.fb, 0, 1, Type::U8), F(mf::flt(j, mf::FlFlat), U.fb + 1, 0, 1, Type::U8);
      if (U.hasWsBip) F(mf::flt(j, mf::FlWsBipolar), U.wsBip, 0, 1, Type::U8);
    }
    for (int a = 0; a < static_cast<int>(U.arts.size()) && a < mf::unitArts(u); ++a) {
      const auto& A = U.arts[static_cast<size_t>(a)];
      F(mf::art(u, a, mf::AfEnvOn), A.envTail + 4, -1, 1), F(mf::art(u, a, mf::AfLfoOn), A.lfoTail + 4, -1, 1);
      F(mf::art(u, a, mf::AfAtt), A.p1 + 4, 0, 256), F(mf::art(u, a, mf::AfDec), A.p1 + 8, 0, 256);
      F(mf::art(u, a, mf::AfSus), A.p1 + 12, -128, 128), F(mf::art(u, a, mf::AfRel), A.p1 + 16, 0, 256);
      F(mf::art(u, a, mf::AfTempo), A.lfoTail, 0, 3), F(mf::art(u, a, mf::AfLfoGlobal), A.lfoTail, 0, 3);
      F(mf::art(u, a, mf::AfLfoSpeed), A.p2 + 4, 0, 65536), F(mf::art(u, a, mf::AfLfoSkew), A.p2 + 12, -128, 128);
    }
  }
  return f;
}

// The template with Gridline's values put in, in the stored form for the fields.
void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  Bytes raw;
  if (b.size() < 8 || !inflate(b, raw)) return;
  Patch p;
  const uint32_t head = getU32(b, 0);
  if (!parse(raw, head, p)) {
    // A template the structure does not read: the fields alone (stored form).
    b = storedForm(head, raw);
    return;
  }
  apply(g, p);
  b = storedForm(p.head, serialize(p));
}

// Compressed again, with the adler-32 of the final patch.
void finish(Bytes& b) {
  Bytes raw;
  if (b.size() < 10 || !inflate(b, raw) || !isStored(b, raw.size())) return;
  b = compressed(getU32(b, 0), raw);
}

Bytes defaults() {
  static const Bytes d = [] {
    Patch p = defaultPatch();
    apply(defaultPluginState("gl.matrixfm"), p);
    return compressed(p.head, serialize(p));
  }();
  return d;
}

}  // namespace

// Lossless structure check (tests): parse + serialize of an FL Sytrus state.
bool sytrusStructureRoundTrip(const Bytes& state, Bytes* rawOut, Bytes* rewritten) {
  Bytes raw;
  if (state.size() < 8 || !inflate(state, raw)) return false;
  Patch p;
  if (!parse(raw, getU32(state, 0), p)) return false;
  const Bytes again = serialize(p);
  if (rawOut) *rawOut = raw;
  if (rewritten) *rewritten = again;
  return again == raw;
}

// Uncompressed patch of a Sytrus state without its info text (comparisons;
// empty when it does not read).
Bytes sytrusRaw(const Bytes& state) {
  Bytes raw;
  if (state.size() < 8 || !inflate(state, raw)) return {};
  Patch p;
  if (!parse(raw, getU32(state, 0), p)) return {};
  p.info.clear();
  return serialize(p);
}

Encoder sytrusEncoder() {
  Encoder e;
  e.flName = "Sytrus";
  e.glType = "gl.matrixfm";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  e.finish = finish;
  return e;
}

}  // namespace gl::flp::enc

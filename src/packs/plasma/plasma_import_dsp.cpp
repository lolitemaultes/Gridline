// Gridline: "plasma" pack, opening the user's own Serum presets
// (.SerumPreset, Serum 1 .fxp through plasma_serum_dsp.cpp) as a Plasma
// state. The Serum 2 container is a small JSON header
// followed by a zstd frame holding a CBOR map of module states; both decoders
// are implemented here (RFC 8878 zstd, RFC 8949 CBOR). Values are mapped
// module by module onto Plasma's parameters and data model; wavetables,
// samples, multisample maps and impulse responses referenced by the preset are
// loaded from the user's own content folders at runtime (nothing is bundled).
// Everything that cannot be reproduced exactly is listed in the report.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include "../../core/Json.h"
#include "../../core/Platform.h"
#include "plasma.h"
#include "plasma_serum.h"

namespace gl::packs::plasma {

namespace fs = std::filesystem;

// ================================================================== zstd (RFC 8878), decode only
namespace {

struct ZErr {};

inline int highBit(uint32_t v) {
  int r = -1;
  while (v) v >>= 1, ++r;
  return r;
}

// Backward bit reader (FSE / Huffman streams): the last byte holds a 1-bit end marker.
struct BackBits {
  const uint8_t* p = nullptr;
  int64_t pos = 0;  // bits left above position 0
  void init(const uint8_t* src, size_t n) {
    if (n == 0) throw ZErr{};
    p = src;
    const uint8_t last = src[n - 1];
    if (last == 0) throw ZErr{};
    pos = static_cast<int64_t>(n - 1) * 8 + highBit(last);
  }
  uint64_t get(int n) {
    if (n == 0) return 0;
    uint64_t v = 0;
    const int64_t start = pos - n;
    for (int i = 0; i < n; ++i) {
      const int64_t b = start + i;
      if (b >= 0 && ((p[b >> 3] >> (b & 7)) & 1)) v |= uint64_t(1) << i;
    }
    pos -= n;
    return v;
  }
  uint64_t peek(int n) const {
    BackBits c = *this;
    return c.get(n);
  }
};
// Forward bit reader (FSE table descriptions).
struct FwdBits {
  const uint8_t* p;
  size_t n, bit = 0;
  uint32_t get(int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; ++i, ++bit) {
      if ((bit >> 3) >= n) throw ZErr{};
      if ((p[bit >> 3] >> (bit & 7)) & 1) v |= 1u << i;
    }
    return v;
  }
  uint32_t peek(int k) const {
    FwdBits c = *this;
    return c.get(k);
  }
};

struct FseEntry {
  uint16_t symbol = 0, baseline = 0;
  uint8_t bits = 0;
};
struct FseTable {
  int log = 0;
  std::vector<FseEntry> t;
};
void fseBuild(FseTable& T, const int16_t* norm, int nsym, int log) {
  const int size = 1 << log;
  T.log = log;
  T.t.assign(static_cast<size_t>(size), FseEntry{});
  std::vector<uint16_t> next(static_cast<size_t>(std::max(nsym, 1)));
  int high = size - 1;
  for (int s = 0; s < nsym; ++s)
    if (norm[s] == -1) {
      T.t[static_cast<size_t>(high--)].symbol = static_cast<uint16_t>(s);
      next[static_cast<size_t>(s)] = 1;
    }
  const int step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
  int pos = 0;
  for (int s = 0; s < nsym; ++s) {
    if (norm[s] <= 0) continue;
    next[static_cast<size_t>(s)] = static_cast<uint16_t>(norm[s]);
    for (int i = 0; i < norm[s]; ++i) {
      T.t[static_cast<size_t>(pos)].symbol = static_cast<uint16_t>(s);
      do pos = (pos + step) & mask;
      while (pos > high);
    }
  }
  if (pos != 0) throw ZErr{};
  for (int i = 0; i < size; ++i) {
    FseEntry& e = T.t[static_cast<size_t>(i)];
    const uint32_t x = next[e.symbol]++;
    const int nb = log - highBit(x);
    e.bits = static_cast<uint8_t>(nb);
    e.baseline = static_cast<uint16_t>((x << nb) - static_cast<uint32_t>(size));
  }
}
// Reads an FSE table description; returns bytes consumed.
size_t fseRead(FseTable& T, const uint8_t* src, size_t n, int maxSym, int maxLog) {
  FwdBits b{src, n};
  const int log = static_cast<int>(b.get(4)) + 5;
  if (log > maxLog) throw ZErr{};
  int remaining = 1 << log;
  std::vector<int16_t> norm(static_cast<size_t>(maxSym + 1), 0);
  int s = 0;
  while (remaining > 0 && s <= maxSym) {
    const int bits = highBit(static_cast<uint32_t>(remaining + 1)) + 1;
    uint32_t val = b.peek(bits);
    const uint32_t lower = (1u << (bits - 1)) - 1;
    const uint32_t thr = (1u << bits) - 1 - static_cast<uint32_t>(remaining + 1);
    if ((val & lower) < thr) {
      b.get(bits - 1);
      val &= lower;
    } else if (val > lower) {
      b.get(bits);
      val -= thr;
    } else {
      b.get(bits);
    }
    const int prob = static_cast<int>(val) - 1;
    remaining -= prob < 0 ? -prob : prob;
    norm[static_cast<size_t>(s++)] = static_cast<int16_t>(prob);
    if (prob == 0) {
      for (;;) {
        const uint32_t rep = b.get(2);
        for (uint32_t i = 0; i < rep && s <= maxSym; ++i) norm[static_cast<size_t>(s++)] = 0;
        if (rep != 3) break;
      }
    }
  }
  if (remaining != 0) throw ZErr{};
  fseBuild(T, norm.data(), s, log);
  return (b.bit + 7) / 8;
}

struct Huff {
  int maxBits = 0;
  std::vector<uint8_t> sym, bits;
};
size_t huffRead(Huff& H, const uint8_t* src, size_t n) {
  if (n < 1) throw ZErr{};
  std::vector<uint8_t> w;
  size_t used;
  const int hb = src[0];
  if (hb >= 128) {
    const int cnt = hb - 127;
    used = 1 + static_cast<size_t>((cnt + 1) / 2);
    if (used > n) throw ZErr{};
    for (int i = 0; i < cnt; ++i) {
      const uint8_t by = src[1 + i / 2];
      w.push_back(static_cast<uint8_t>(i % 2 == 0 ? by >> 4 : by & 15));
    }
  } else {
    used = 1 + static_cast<size_t>(hb);
    if (used > n) throw ZErr{};
    FseTable T;
    const size_t h = fseRead(T, src + 1, static_cast<size_t>(hb), 255, 6);
    BackBits br;
    br.init(src + 1 + h, static_cast<size_t>(hb) - h);
    uint32_t s1 = static_cast<uint32_t>(br.get(T.log)), s2 = static_cast<uint32_t>(br.get(T.log));
    for (;;) {
      if (w.size() > 255) throw ZErr{};
      const FseEntry& e1 = T.t[s1];
      w.push_back(static_cast<uint8_t>(e1.symbol));
      s1 = e1.baseline + static_cast<uint32_t>(br.get(e1.bits));
      if (br.pos < 0) {
        w.push_back(static_cast<uint8_t>(T.t[s2].symbol));
        break;
      }
      const FseEntry& e2 = T.t[s2];
      w.push_back(static_cast<uint8_t>(e2.symbol));
      s2 = e2.baseline + static_cast<uint32_t>(br.get(e2.bits));
      if (br.pos < 0) {
        w.push_back(static_cast<uint8_t>(T.t[s1].symbol));
        break;
      }
    }
  }
  uint32_t sum = 0;
  for (uint8_t x : w) {
    if (x > 11) throw ZErr{};
    if (x) sum += 1u << (x - 1);
  }
  if (sum == 0) throw ZErr{};
  const int maxBits = highBit(sum) + 1;
  const uint32_t left = (1u << maxBits) - sum;
  if (left & (left - 1)) throw ZErr{};
  w.push_back(static_cast<uint8_t>(highBit(left) + 1));
  if (maxBits > 11) throw ZErr{};
  H.maxBits = maxBits;
  const size_t size = size_t(1) << maxBits;
  H.sym.assign(size, 0);
  H.bits.assign(size, 0);
  std::array<uint32_t, 13> rankCount{}, rankIdx{};
  std::vector<uint8_t> nb(w.size());
  for (size_t i = 0; i < w.size(); ++i) {
    nb[i] = w[i] ? static_cast<uint8_t>(maxBits + 1 - w[i]) : 0;
    rankCount[nb[i]]++;
  }
  rankIdx[static_cast<size_t>(maxBits)] = 0;
  for (int i = maxBits; i >= 1; --i) rankIdx[static_cast<size_t>(i - 1)] = rankIdx[static_cast<size_t>(i)] + rankCount[static_cast<size_t>(i)] * (1u << (maxBits - i));
  for (size_t s = 0; s < w.size(); ++s) {
    if (!nb[s]) continue;
    const uint32_t code = rankIdx[nb[s]];
    const uint32_t len = 1u << (maxBits - nb[s]);
    if (code + len > size) throw ZErr{};
    for (uint32_t i = 0; i < len; ++i) H.sym[code + i] = static_cast<uint8_t>(s), H.bits[code + i] = nb[s];
    rankIdx[nb[s]] += len;
  }
  return used;
}
void huffStream(const Huff& H, const uint8_t* src, size_t n, uint8_t* out, size_t count) {
  BackBits br;
  br.init(src, n);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t idx = static_cast<uint32_t>(br.peek(H.maxBits));
    out[i] = H.sym[idx];
    br.get(H.bits[idx]);
  }
}

const uint32_t kLLBase[36] = {0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   10,   11,   12,   13,   14,   15,    16,    18,
                              20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLLBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMLBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12,  13,  14,  15,  16,  17,   18,   19,   20,
                              21, 22, 23, 24, 25, 26, 27, 28, 29, 30,  31,  32,  33,  34,  35,   37,   39,   41,
                              43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMLBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const int16_t kLLDef[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMLDef[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOFDef[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

struct ZState {
  Huff huff;
  bool hasHuff = false;
  FseTable ll, of, ml;
  bool hasLL = false, hasOF = false, hasML = false;
  uint32_t rep[3] = {1, 4, 8};
};

// Literals section; returns bytes used.
size_t literals(ZState& z, const uint8_t* src, size_t n, std::vector<uint8_t>& lit) {
  if (n < 1) throw ZErr{};
  const int type = src[0] & 3, fmt = (src[0] >> 2) & 3;
  if (type <= 1) {
    size_t hdr, size;
    if (fmt == 0 || fmt == 2) hdr = 1, size = src[0] >> 3;
    else if (fmt == 1) {
      if (n < 2) throw ZErr{};
      hdr = 2, size = (src[0] >> 4) + (static_cast<size_t>(src[1]) << 4);
    } else {
      if (n < 3) throw ZErr{};
      hdr = 3, size = (src[0] >> 4) + (static_cast<size_t>(src[1]) << 4) + (static_cast<size_t>(src[2]) << 12);
    }
    if (type == 0) {
      if (hdr + size > n) throw ZErr{};
      lit.assign(src + hdr, src + hdr + size);
      return hdr + size;
    }
    if (hdr + 1 > n) throw ZErr{};
    lit.assign(size, src[hdr]);
    return hdr + 1;
  }
  size_t hdr, regen, comp;
  int streams = fmt == 0 ? 1 : 4;
  if (fmt <= 1) {
    if (n < 3) throw ZErr{};
    const uint32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
    hdr = 3, regen = (v >> 4) & 0x3FF, comp = (v >> 14) & 0x3FF;
  } else if (fmt == 2) {
    if (n < 4) throw ZErr{};
    const uint32_t v = src[0] | (src[1] << 8) | (src[2] << 16) | (static_cast<uint32_t>(src[3]) << 24);
    hdr = 4, regen = (v >> 4) & 0x3FFF, comp = (v >> 18) & 0x3FFF;
  } else {
    if (n < 5) throw ZErr{};
    const uint64_t v = src[0] | (src[1] << 8) | (src[2] << 16) | (static_cast<uint64_t>(src[3]) << 24) | (static_cast<uint64_t>(src[4]) << 32);
    hdr = 5, regen = (v >> 4) & 0x3FFFF, comp = (v >> 22) & 0x3FFFF;
  }
  if (hdr + comp > n) throw ZErr{};
  const uint8_t* p = src + hdr;
  size_t left = comp;
  if (type == 2) {
    const size_t u = huffRead(z.huff, p, left);
    z.hasHuff = true;
    p += u, left -= u;
  } else if (!z.hasHuff) {
    throw ZErr{};
  }
  lit.assign(regen, 0);
  if (streams == 1) {
    huffStream(z.huff, p, left, lit.data(), regen);
  } else {
    if (left < 6) throw ZErr{};
    const size_t s1 = p[0] | (p[1] << 8), s2 = p[2] | (p[3] << 8), s3 = p[4] | (p[5] << 8);
    if (6 + s1 + s2 + s3 > left) throw ZErr{};
    const size_t s4 = left - 6 - s1 - s2 - s3;
    const size_t seg = (regen + 3) / 4;
    if (seg * 3 > regen) throw ZErr{};
    const uint8_t* q = p + 6;
    huffStream(z.huff, q, s1, lit.data(), seg);
    huffStream(z.huff, q + s1, s2, lit.data() + seg, seg);
    huffStream(z.huff, q + s1 + s2, s3, lit.data() + 2 * seg, seg);
    huffStream(z.huff, q + s1 + s2 + s3, s4, lit.data() + 3 * seg, regen - 3 * seg);
  }
  return hdr + comp;
}

size_t seqTable(FseTable& T, bool& has, int mode, const uint8_t* src, size_t n, const int16_t* def, int defN, int defLog, int maxSym,
                int maxLog) {
  switch (mode) {
    case 0:
      fseBuild(T, def, defN, defLog);
      has = true;
      return 0;
    case 1: {
      if (n < 1) throw ZErr{};
      T.log = 0;
      T.t.assign(1, FseEntry{});
      T.t[0].symbol = src[0];
      has = true;
      return 1;
    }
    case 2: {
      const size_t u = fseRead(T, src, n, maxSym, maxLog);
      has = true;
      return u;
    }
    default:
      if (!has) throw ZErr{};
      return 0;
  }
}

void block(ZState& z, const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t cap) {
  std::vector<uint8_t> lit;
  size_t p = literals(z, src, n, lit);
  if (p >= n) {  // no sequences byte: literals only (allowed when the block ends here)
    out.insert(out.end(), lit.begin(), lit.end());
    return;
  }
  size_t nseq = src[p++];
  if (nseq >= 128) {
    if (nseq < 255) {
      if (p >= n) throw ZErr{};
      nseq = ((nseq - 128) << 8) + src[p++];
    } else {
      if (p + 2 > n) throw ZErr{};
      nseq = src[p] + (static_cast<size_t>(src[p + 1]) << 8) + 0x7F00;
      p += 2;
    }
  }
  if (nseq == 0) {
    out.insert(out.end(), lit.begin(), lit.end());
    return;
  }
  if (p >= n) throw ZErr{};
  const uint8_t modes = src[p++];
  p += seqTable(z.ll, z.hasLL, modes >> 6, src + p, n - p, kLLDef, 36, 6, 35, 9);
  p += seqTable(z.of, z.hasOF, (modes >> 4) & 3, src + p, n - p, kOFDef, 29, 5, 31, 8);
  p += seqTable(z.ml, z.hasML, (modes >> 2) & 3, src + p, n - p, kMLDef, 53, 6, 52, 9);
  if (p >= n) throw ZErr{};
  BackBits br;
  br.init(src + p, n - p);
  uint32_t sLL = static_cast<uint32_t>(br.get(z.ll.log)), sOF = static_cast<uint32_t>(br.get(z.of.log)), sML = static_cast<uint32_t>(br.get(z.ml.log));
  size_t litPos = 0;
  for (size_t i = 0; i < nseq; ++i) {
    if (sLL >= z.ll.t.size() || sOF >= z.of.t.size() || sML >= z.ml.t.size()) throw ZErr{};
    const int ofCode = z.of.t[sOF].symbol, mlCode = z.ml.t[sML].symbol, llCode = z.ll.t[sLL].symbol;
    if (ofCode > 31 || mlCode > 52 || llCode > 35) throw ZErr{};
    const uint32_t ofVal = (1u << ofCode) + static_cast<uint32_t>(br.get(ofCode));
    const uint32_t ml = kMLBase[mlCode] + static_cast<uint32_t>(br.get(kMLBits[mlCode]));
    const uint32_t ll = kLLBase[llCode] + static_cast<uint32_t>(br.get(kLLBits[llCode]));
    uint32_t off;
    if (ofVal > 3) {
      off = ofVal - 3;
      z.rep[2] = z.rep[1], z.rep[1] = z.rep[0], z.rep[0] = off;
    } else {
      int idx = static_cast<int>(ofVal) - 1 + (ll == 0 ? 1 : 0);
      if (idx == 0) {
        off = z.rep[0];
      } else {
        off = idx == 3 ? z.rep[0] - 1 : z.rep[idx];
        if (idx > 1) z.rep[2] = z.rep[1];
        z.rep[1] = z.rep[0];
        z.rep[0] = off;
      }
    }
    if (i + 1 < nseq) {
      const FseEntry eLL = z.ll.t[sLL], eML = z.ml.t[sML], eOF = z.of.t[sOF];
      sLL = eLL.baseline + static_cast<uint32_t>(br.get(eLL.bits));
      sML = eML.baseline + static_cast<uint32_t>(br.get(eML.bits));
      sOF = eOF.baseline + static_cast<uint32_t>(br.get(eOF.bits));
    }
    if (litPos + ll > lit.size() || out.size() + ll + ml > cap) throw ZErr{};
    out.insert(out.end(), lit.begin() + static_cast<std::ptrdiff_t>(litPos), lit.begin() + static_cast<std::ptrdiff_t>(litPos + ll));
    litPos += ll;
    if (off == 0 || off > out.size()) throw ZErr{};
    size_t from = out.size() - off;
    for (uint32_t k = 0; k < ml; ++k) out.push_back(out[from + k]);
  }
  out.insert(out.end(), lit.begin() + static_cast<std::ptrdiff_t>(litPos), lit.end());
}

}  // namespace

bool zstdDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out, std::string* err) {
  out.clear();
  constexpr size_t kCap = size_t(512) << 20;
  try {
    size_t p = 0;
    while (p + 4 <= n) {
      const uint32_t magic = src[p] | (src[p + 1] << 8) | (src[p + 2] << 16) | (static_cast<uint32_t>(src[p + 3]) << 24);
      p += 4;
      if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  // skippable frame
        if (p + 4 > n) throw ZErr{};
        const uint32_t len = src[p] | (src[p + 1] << 8) | (src[p + 2] << 16) | (static_cast<uint32_t>(src[p + 3]) << 24);
        p += 4 + len;
        continue;
      }
      if (magic != 0xFD2FB528u) throw ZErr{};
      if (p >= n) throw ZErr{};
      const uint8_t d = src[p++];
      const int fcsFlag = d >> 6, single = (d >> 5) & 1, check = (d >> 2) & 1, dictFlag = d & 3;
      if (d & 8) throw ZErr{};  // reserved bit
      if (!single) ++p;         // window descriptor (the whole output is kept)
      const int dictBytes[4] = {0, 1, 2, 4};
      size_t dictId = 0;
      for (int i = 0; i < dictBytes[dictFlag]; ++i) dictId |= static_cast<size_t>(src[p + static_cast<size_t>(i)]) << (8 * i);
      p += static_cast<size_t>(dictBytes[dictFlag]);
      if (dictId != 0) throw ZErr{};  // dictionaries are not used by these files
      const int fcsBytes = fcsFlag == 0 ? (single ? 1 : 0) : fcsFlag == 1 ? 2 : fcsFlag == 2 ? 4 : 8;
      p += static_cast<size_t>(fcsBytes);
      if (p > n) throw ZErr{};
      ZState z;
      const size_t frameStart = out.size();
      for (;;) {
        if (p + 3 > n) throw ZErr{};
        const uint32_t h = src[p] | (src[p + 1] << 8) | (src[p + 2] << 16);
        p += 3;
        const bool last = h & 1;
        const int type = (h >> 1) & 3;
        const size_t size = h >> 3;
        if (type == 0) {
          if (p + size > n || out.size() + size > kCap) throw ZErr{};
          out.insert(out.end(), src + p, src + p + size);
          p += size;
        } else if (type == 1) {
          if (p + 1 > n || out.size() + size > kCap) throw ZErr{};
          out.insert(out.end(), size, src[p]);
          p += 1;
        } else if (type == 2) {
          if (p + size > n) throw ZErr{};
          block(z, src + p, size, out, kCap);
          p += size;
        } else {
          throw ZErr{};
        }
        if (last) break;
      }
      (void)frameStart;
      if (check) p += 4;
    }
    return true;
  } catch (const ZErr&) {
    if (err) *err = "damaged compressed data";
  } catch (const std::exception&) {
    if (err) *err = "damaged compressed data";
  }
  out.clear();
  return false;
}

// ================================================================== CBOR (RFC 8949) -> Json
namespace {
// Audio arrays (embedded wavetables, samples, noises, impulses) are packed as
// float blobs while decoding: millions of numbers as single Json strings.
bool packedKey(const std::string& k) {
  return k == "embeddedWTData" || k == "embeddedIR" || k == "embeddedNoiseData" || k == "data";
}
struct CborIn {
  const uint8_t* p;
  size_t n, i = 0;
  int depth = 0;
  int pack = 0;  // > 0: inside a packed key
  // A number item at i (integer or float); false (nothing read) when the item is not a number.
  bool number(double& v) {
    if (i >= n) throw ZErr{};
    const uint8_t ib = p[i];
    const int mt = ib >> 5, ai = ib & 31;
    if (mt == 0 || mt == 1) {
      ++i;
      const uint64_t a = arg(ai);
      v = mt == 0 ? static_cast<double>(a) : -1.0 - static_cast<double>(a);
      return true;
    }
    if (mt == 7 && ai >= 25 && ai <= 27) {
      Json j;
      value(j);
      v = j.number;
      return true;
    }
    return false;
  }
  // Numbers until the end of an array (len items, or indefinite: up to 0xFF) as a float blob;
  // false (nothing read) when an item is not a number.
  bool floats(Json& out, bool indef, uint64_t len) {
    const size_t start = i;
    std::string b(serum::kBlobTag);
    b.reserve(b.size() + static_cast<size_t>(std::min<uint64_t>(len, n)) * 4);
    for (uint64_t k = 0; indef || k < len; ++k) {
      if (indef) {
        if (i >= n) throw ZErr{};
        if (p[i] == 0xFF) {
          ++i;
          break;
        }
      }
      double v = 0;
      if (!number(v)) {
        i = start;
        return false;
      }
      const float f = static_cast<float>(v);
      char c[4];
      std::memcpy(c, &f, 4);
      b.append(c, 4);
    }
    out = Json::makeString(std::move(b));
    return true;
  }
  uint8_t byte() {
    if (i >= n) throw ZErr{};
    return p[i++];
  }
  uint64_t arg(int ai) {
    if (ai < 24) return static_cast<uint64_t>(ai);
    int k = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : -1;
    if (k < 0) throw ZErr{};
    uint64_t v = 0;
    for (int j = 0; j < k; ++j) v = (v << 8) | byte();
    return v;
  }
  void value(Json& out) {
    if (++depth > 200) throw ZErr{};
    const uint8_t ib = byte();
    const int mt = ib >> 5, ai = ib & 31;
    switch (mt) {
      case 0: out = Json::makeNumber(static_cast<double>(arg(ai))); break;
      case 1: out = Json::makeNumber(-1.0 - static_cast<double>(arg(ai))); break;
      case 2:
      case 3: {
        std::string s;
        if (ai == 31) {
          for (;;) {
            if (i >= n) throw ZErr{};
            if (p[i] == 0xFF) {
              ++i;
              break;
            }
            Json part;
            value(part);
            s += part.str;
          }
        } else {
          const uint64_t len = arg(ai);
          if (len > n - i) throw ZErr{};
          s.assign(reinterpret_cast<const char*>(p + i), static_cast<size_t>(len));
          i += static_cast<size_t>(len);
        }
        out = Json::makeString(s);
        break;
      }
      case 4: {
        if (pack > 0) {  // a run of numbers becomes a blob; arrays of arrays (channels) recurse
          const bool indef = ai == 31;
          const uint64_t len = indef ? 0 : arg(ai);
          if (!indef && len > n - i) throw ZErr{};
          const bool nums = i < n && p[i] != 0xFF && (len > 0 || indef) && ((p[i] >> 5) <= 1 || (p[i] >= 0xF9 && p[i] <= 0xFB));
          if (nums && floats(out, indef, len)) break;  // else (a mixed array): read as usual
          out = Json::makeArray();
          for (uint64_t k = 0; indef || k < len; ++k) {
            if (indef) {
              if (i >= n) throw ZErr{};
              if (p[i] == 0xFF) {
                ++i;
                break;
              }
            }
            out.array.emplace_back();
            value(out.array.back());
          }
          break;
        }
        out = Json::makeArray();
        if (ai == 31) {
          for (;;) {
            if (i >= n) throw ZErr{};
            if (p[i] == 0xFF) {
              ++i;
              break;
            }
            out.array.emplace_back();
            value(out.array.back());
          }
        } else {
          const uint64_t len = arg(ai);
          if (len > n - i) throw ZErr{};
          out.array.reserve(static_cast<size_t>(len));
          for (uint64_t k = 0; k < len; ++k) {
            out.array.emplace_back();
            value(out.array.back());
          }
        }
        break;
      }
      case 5: {
        out = Json::makeObject();
        const bool indef = ai == 31;
        const uint64_t len = indef ? 0 : arg(ai);
        if (!indef && len > n - i) throw ZErr{};
        for (uint64_t k = 0; indef || k < len; ++k) {
          if (indef) {
            if (i >= n) throw ZErr{};
            if (p[i] == 0xFF) {
              ++i;
              break;
            }
          }
          Json key, v;
          value(key);
          const bool pk = key.type == Json::Type::String && packedKey(key.str);
          const int outer = pack;  // only the packed key's own value (and its channel arrays) is packed
          pack = pk ? 1 : 0;
          value(v);
          pack = outer;
          std::string ks = key.type == Json::Type::String ? key.str : key.type == Json::Type::Number ? std::to_string(static_cast<long long>(key.number)) : "?";
          out.object[ks] = std::move(v);
        }
        break;
      }
      case 6:
        arg(ai);
        value(out);
        break;
      default: {
        if (ai == 20) out = Json(), out.type = Json::Type::Bool, out.boolean = false;
        else if (ai == 21) out = Json(), out.type = Json::Type::Bool, out.boolean = true;
        else if (ai == 22 || ai == 23) out = Json();
        else if (ai == 25) {
          const uint16_t h = static_cast<uint16_t>(arg(25));
          const int e = (h >> 10) & 31, m = h & 1023;
          double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(m + 1024, e - 25);
          out = Json::makeNumber(h & 0x8000 ? -v : v);
        } else if (ai == 26) {
          const uint32_t u = static_cast<uint32_t>(arg(26));
          float f;
          std::memcpy(&f, &u, 4);
          out = Json::makeNumber(static_cast<double>(f));
        } else if (ai == 27) {
          const uint64_t u = arg(27);
          double d;
          std::memcpy(&d, &u, 8);
          out = Json::makeNumber(d);
        } else if (ai < 20) {
          out = Json();
        } else {
          throw ZErr{};
        }
        break;
      }
    }
    --depth;
  }
};
}  // namespace

bool cborDecode(const uint8_t* src, size_t n, Json& out, std::string* err) {
  try {
    CborIn c{src, n};
    c.value(out);
    return true;
  } catch (...) {
    if (err) *err = "damaged preset data";
  }
  return false;
}

// ================================================================== preset mapping
namespace {

const char* const kFilterKeys[] = {
    "MgL6",     "MgL12",    "MgL18",       "MgL24",       "L6",          "L12",         "L18",         "L24",        "H6",
    "H12",      "H18",      "H24",         "B12",         "B24",         "P12",         "P24",         "N12",        "N24",
    "LH6",      "LH12",     "LB12",        "LP12",        "LN12",        "HB12",        "HP12",        "HN12",       "BP12",
    "BN12",     "PP12",     "PN12",        "NN12",        "LBH12",       "LBH24",       "LPH12",       "LPH24",      "LNH12",
    "LNH24",    "BPN12",    "BPN24",       "CombP",       "CombN",       "CombL6P",     "CombL6N",     "CombH6P",    "CombH6N",
    "CombHL6P", "CombHL6N", "FlangeP",     "FlangeN",     "FlangeL6P",   "FlangeL6N",   "FlangeH6P",   "FlangeH6N",  "FlangeHL6P",
    "FlangeHL6N", "Phase12P", "Phase12N",  "Phase24P",    "Phase24N",    "Phase36P",    "Phase36N",    "Phase48P",   "Phase48N",
    "Phase48L6P", "Phase48L6N", "Phase48H6P", "Phase48H6N", "Phase48HL6P", "Phase48HL6N", "FlangePhase12HL6P", "FlangePhase12HL6N", "LEQ6",
    "LEQ12",    "BEQ12",    "HEQ6",        "HEQ12",       "RM",          "RMT",         "SNH1",        "SNH2",       "Combs",
    "Allpasses", "Reverb1", "Scream",      "ZDF_A",       "ADD_BASS",    "FormantONE",  "FormantTWO",  "FormantTWB", "BandReject",
    "DistComb1LP", "DistComb1BP", "DistComb2LP", "DistComb2BP", "Scream3LP", "Scream3BP", "Wsp",       "DJMixer",    "Diffuser",
    "LadderMg", "LadderAcid", "LadderEMS",  "DirtyMg",     "PZ_SVF",      "Comb2",       "Exp",         "ExpBPF"};
constexpr int kFilterKeyCount = static_cast<int>(sizeof(kFilterKeys) / sizeof(kFilterKeys[0]));

// Warp tokens of the wavetable list, in Plasma's warp index order 0..69.
const char* const kWarpKeys[] = {"kNoWarp",      "kSync",        "kBendPos",       "kBendNeg",      "kBendPosNeg",   "kPWM",
                                 "kASYMPos",     "kASYMNeg",     "kASYMPosNeg",    "kFlip",         "kDLM",          "kRemap_1",
                                 "kRemap_2",     "kRemap_3",     "kRemap_4",       "kQuantize",     "kEvenOdd",      "kFilterLPF",
                                 "kFilterHPF",   "kDistTube",    "kDistSoftClip",  "kDistHardClip", "kDistDiode1",   "kDistDiode2",
                                 "kDistLinFold", "kDistSinFold", "kDistZeroSquare", "kDistAsym",    "kDistRectify",  "kDistSineShaper",
                                 "kDistStompBox", "kDistTapeSat", "kDistSoftSat",  "kFM_OSC",       "kFM_OSC2",      "kFM_NOISE",
                                 "kFM_SUB",      "kFM_FILT1",    "kFM_FILT2",      "kFMX_OSC",      "kFMX_OSC2",     "kFMX_NOISE",
                                 "kFMX_SUB",     "kFMX_FILT1",   "kFMX_FILT2",     "kFMP_OSC",      "kFMP_OSC2",     "kFMP_NOISE",
                                 "kFMP_SUB",     "kFMP_FILT1",   "kFMP_FILT2",     "kPD_OSC",       "kPD_OSC2",      "kPD_NOISE",
                                 "kPD_SUB",      "kPD_FILT1",    "kPD_FILT2",      "kSelfPD",       "kAM_OSC",       "kAM_OSC2",
                                 "kAM_NOISE",    "kAM_SUB",      "kAM_FILT1",      "kAM_FILT2",     "kRM_OSC",       "kRM_OSC2",
                                 "kRM_NOISE",    "kRM_SUB",      "kRM_FILT1",      "kRM_FILT2"};
// Spectral-only tokens, Plasma warp index 70..103.
const char* const kSpecWarpKeys[] = {"kDetune",       "kSmear",          "kSpread",         "kAddharmonics",    "kAddsubharmonics",
                                     "kGate",         "kRobotize",       "kSpectralShift",  "kMirror",          "kPeakFollow",
                                     "kPeakOctaveUp", "kPeakOctaveDown", "kPeakHarmUp",     "kPeakHarmDown",    "kPeakHarmSweep",
                                     "kShepardNarrow", "kShepardFilter", "kSpectralComb",   "kSpectralPitchShift", "kSpectralPitchShiftNew",
                                     "kSpectralPhaseTwist", "kSpectralFormantShift", "kMask_OSC", "kMask_OSC2", "kMask_NOISE",
                                     "kMask_SUB",     "kMask_FILT1",     "kMask_FILT2",     "kVocode_OSC",      "kVocode_OSC2",
                                     "kVocode_NOISE", "kVocode_SUB",     "kVocode_FILT1",   "kVocode_FILT2"};

int indexOf(const char* const* keys, int n, const std::string& s) {
  for (int i = 0; i < n; ++i)
    if (s == keys[i]) return i;
  return -1;
}
int warpIndex(const std::string& s, int oscType) {
  if (oscType == OscSpectral) {
    const int k = indexOf(kSpecWarpKeys, 34, s);
    if (k >= 0) return 70 + k;
  }
  const int w = indexOf(kWarpKeys, 70, s);
  if (w >= 0) return w;
  const int k = indexOf(kSpecWarpKeys, 34, s);
  return k >= 0 ? 70 + k : -1;
}

// Serum modulation source ids -> Plasma sources.
int sourceOf(int id) {
  if (id <= 0) return SrcNone;
  if (id == 1) return SrcModWheel;
  if (id >= 2 && id <= 5) return SrcEnv1 + (id - 2);
  if (id >= 6 && id <= 15) return SrcLfo1 + (id - 6);
  if (id >= 25 && id <= 32) return SrcMacro1 + (id - 25);
  if (id >= 39 && id <= 48) return SrcLfo1Y + (id - 39);
  switch (id) {
    case 16: return SrcVelo;
    case 17: return SrcNote;
    case 18: return SrcAftertouch;
    case 19: return SrcPitchBend;
    case 20: return SrcNoise;
    case 21: return SrcRand1;
    case 22: return SrcRand2;
    case 23: return SrcAlt1;
    case 24: return SrcAlt2;
    case 33: return SrcPolyAT;
    case 34: return SrcExprX;
    case 35: return SrcExprY;
    case 36: return SrcExprZ;
    case 37: return SrcReleaseVelo;
    case 38: return SrcFixed;
    case 49: return SrcOscA;
    case 50: return SrcOscB;
    case 51: return SrcOscC;
    case 52: return SrcSub;
    case 53: return SrcFilter1;
    case 54: return SrcFilter2;
    case 55: return SrcActiveVoices;
    case 56: return SrcVoiceMod1;
    case 57: return SrcVoiceMod2;
    case 58: return SrcVoiceIndex;
    case 59: return SrcRandDiscrete;
    default: return -1;
  }
}

enum Conv { Copy, Pct, Pan50, Deg, Bool, EnvT, Porta, Sqrt, Div64, Pct2, LfoRate, PctCube, Detune };
double conv(Conv c, double v) {
  switch (c) {
    case Pct: return v / 100.0;
    case Pct2: return v / 100.0;  // -200..200 % -> -2..2
    case Pan50: return v / 50.0;
    case Deg: return v / 360.0;
    case Bool: return v >= 0.5 ? 1 : 0;
    case EnvT: return envTimeInv(std::max(0.0, v));
    case Porta: return portaTimeInv(std::max(0.0, v));
    case Sqrt: return std::sqrt(std::max(0.0, v));
    // J-201: the source's unison spread is +-range * stored^1.24 (measured with two voices at key 60,
    // range 2: stored 0.25 -> +-0.36 st, 0.5 -> +-0.85 st, 1 -> +-2 st; range 12, 0.25 -> +-2.1 st);
    // Plasma's is +-range * knob^2, so knob = stored^0.62
    case Detune: return std::pow(std::max(0.0, v), 0.62);
    case Div64: return v / 64.0;
    case PctCube: return std::pow(std::clamp(v / 100.0, 0.0, 1.0), 3.0);  // bus sends: the source's gain is the knob cubed
    case LfoRate: return lfoHzInv(std::clamp(v, 0.0, 100.0), false);  // Hz (free) / the same knob curve (tempo divisions)
    default: return v;
  }
}
struct KeyMap {
  const char* key;
  int param;  // offset inside the module block
  Conv c;
};

const KeyMap kOscMain[] = {{"kParamEnable", O_Enable, Bool},        {"kParamVolume", O_Level, Sqrt},
                           {"kParamPan", O_Pan, Pan50},             {"kParamOctave", O_Octave, Copy},
                           {"kParamPitch", O_Semi, Copy},           {"kParamFine", O_Fine, Copy},
                           {"kParamCoarsePit", O_Coarse, Copy},     {"kParamPitchRatio", O_Ratio, Copy},
                           {"kParamHzOffset", O_HzOffset, Copy},    {"kParamPitchTrack", O_PitchTrack, Bool},
                           {"kParamUnison", O_Unison, Copy},        {"kParamDetune", O_UniDetune, Detune},
                           {"kParamDetuneWid", O_UniBlend, Pct},    {"kParamUnisonStereo", O_UniWidth, Pct},
                           {"kParamUnisonSpan", O_UniSpan, Pct},    {"kParamUnisonRange", O_UniRange, Copy},
                           {"kParamRandomStart", O_UniRandStart, Pct}, {"kParamUnisonWarp", O_UniWarp1, Pct},
                           {"kParamUnisonWarp2", O_UniWarp2, Pct},  {"kParamStart", O_Start, Pct},
                           {"kParamEnd", O_End, Pct},               {"kParamReverse", O_Reverse, Bool},
                           {"kParamScanRate", O_ScanRate, Pct2},    {"kParamScanTempoLock", O_ScanSync, Bool},
                           {"kParamScanRange", O_ScanKey, Bool},    {"kParamPosition", O_Position, Pct},
                           {"kParamLoopStart", O_LoopStart, Pct},   {"kParamLoopEnd", O_LoopEnd, Pct},
                           {"kParamLoopCrossfade", O_LoopXfade, Pct}, {"kParamLoopStartLink", O_RelLoop, Bool},
                           {"kParamKeyZoneMin", O_KeyMin, Copy},    {"kParamKeyZoneMax", O_KeyMax, Copy},
                           {"kParamVelocityZoneMin", O_VelMin, Copy}, {"kParamVelocityZoneMax", O_VelMax, Copy},
                           {"kParamKeyZoneFold", O_MapFold, Bool},  {"kParamKeyZoneWarp", O_MapWarp, Bool}};
const KeyMap kOscSub[] = {{"kParamWarp", O_Warp1, Copy},           {"kParamWarp2", O_Warp2, Copy},
                          {"kParamWarpVar", O_Warp1Var, Copy},     {"kParamWarpVar2", O_Warp2Var, Copy},
                          {"kParamInitialPhase", O_Phase, Deg},    {"kParamRandomPhase", O_RandPhase, Pct},
                          {"kParamUnisonWTPos", O_UniWtPos, Pct},  {"kParamXfadeMode", O_Smooth, Bool},
                          {"kParamTimbreShift", O_MTimbre, Div64}, {"kParamVelTrack", O_MVelTrack, Pct},
                          {"kParamEnvOverride", O_MEnvOn, Bool},   {"kParamEnvDelay", O_MDelay, EnvT},
                          {"kParamEnvAttack", O_MAttack, EnvT},    {"kParamEnvHold", O_MHold, EnvT},
                          {"kParamEnvDecay", O_MDecay, EnvT},      {"kParamEnvSustain", O_MSustain, Copy},
                          {"kParamEnvRelease", O_MRelease, EnvT},  {"kParamRandomOffset", O_GRndOffset, Pct},
                          {"kParamRandomDir", O_GRndDir, Pct},     {"kParamRandomGrainLength", O_GRndLength, Pct},
                          {"kParamRandomPan", O_GRndPan, Pct},     {"kParamRandomGain", O_GRndGain, Pct},
                          {"kParamRandomWindowAmount", O_GRndWin, Pct}, {"kParamRandomWindowSkew", O_GRndSkew, Pct},
                          {"kParamRandomWarp", O_GRndWarp, Pct},   {"kParamRandomWarp2", O_GRndWarp2, Pct},
                          {"kParamWindowSkew", O_GWinSkew, Pct},   {"kParamWindowParam", O_GWinParam, Pct},
                          {"kParamSpecFltShift", O_SCut, Pct},     {"kParamSpecFltWetDry", O_SMix, Pct},
                          {"kParamPhaseLock", O_SPhaseLock, Bool}, {"kParamTransients", O_STransients, Bool}};
const KeyMap kFiltMap[] = {{"kParamEnable", F_Enable, Bool},  {"kParamFreq", F_Freq, Copy},     {"kParamReso", F_Res, Pct},
                           {"kParamDrive", F_Drive, Pct},     {"kParamVar", F_Var, Pct},        {"kParamStereo", F_Stereo, Pct},
                           {"kParamWet", F_Wet, Pct},         {"kParamLevelOut", F_Level, Copy}, {"kParamKeyTrack", F_KeyTrack, Bool}};
// (the source's filter X / Y have no measurable effect on any of its filter types, J-203b: not mapped)
const KeyMap kEnvMap[] = {{"kParamAttack", E_Attack, EnvT},   {"kParamHold", E_Hold, EnvT},     {"kParamDecay", E_Decay, EnvT},
                          {"kParamSustain", E_Sustain, Copy}, {"kParamRelease", E_Release, EnvT}, {"kParamCurve1", E_CurveA, Copy},
                          {"kParamCurve2", E_CurveD, Copy},   {"kParamCurve3", E_CurveR, Copy}, {"kParamStart", E_Start, Copy},
                          {"kParamEnd", E_End, Copy},         {"kParamBeatSync", E_Bpm, Bool},  {"kParamLegatoInverted", E_LegatoInv, Bool}};
const KeyMap kLfoMap[] = {{"kParamRate", L_Rate, LfoRate},        {"kParamSmooth", L_Smooth, Pct},   {"kParamPhase", L_Phase, Deg},
                          {"kParamBeatSync", L_Bpm, Bool},    {"kParamAnchored", L_Anchor, Bool}, {"kParamDotted", L_Dot, Bool},
                          {"kParamTriplets", L_Trip, Bool},   {"kParamGridX", L_GridX, Copy},    {"kParamGridY", L_GridY, Copy},
                          {"kParamMono", L_Mono, Bool},       {"kParamDirection", L_Dir, Copy},  {"kParamRate10x", L_Rate10x, Bool},
                          {"kParamSwing", L_Swing, Bool},     {"kParamPhaseSnap", L_PhaseSnap, Bool}};
const KeyMap kRouteMap[] = {{"kParamFilterBalance", R_Balance, Pct}, {"kParamFXBus1Level", R_Bus1, PctCube}, {"kParamFXBus2Level", R_Bus2, PctCube}};

struct Importer {
  PluginState& st;
  ImportReport& rep;
  Model m;
  std::vector<std::string> roots;
  std::vector<std::string> near;  // the preset's folder and those above it (direct lookups only)
  bool files = true;              // false: no file lookups (automation link probes)
  std::set<std::string> notes;
  std::map<int, int> fxSlotOf;  // Serum FX instance id (rack * 100 + index) -> Plasma slot
  std::array<int, 3> oscType{};
  std::array<int, 3> oscFrames{{1, 1, 1}};
  double masterTuneSemis = 0.0;     // main tuning, whole semitones (folded into Transpose)
  double noisePitchModScale = 0.75;
  bool builtinNoise = false;          // the noise is a built-in type (its pitch knob is a tone filter)
  bool geigerNoise = false;           // the knob is a density there: routes to it are dropped
  static constexpr double kNoiseLevelTrim = 1.0;  // Plasma noise pitch range per unit of the source's knob (matrix amounts)
  static constexpr double kNoiseDetuneRef = 0.4758218478413068;
  const PluginInfo* info = findPlugin(kType);

  Importer(PluginState& s, ImportReport& r) : st(s), rep(r) {}
  void note(const std::string& s) { notes.insert(s); }
  void set(int k, double v) {
    if (k < 0 || k >= kParamCount) return;
    const ParamDesc& d = info->params[static_cast<size_t>(k)];
    const double hi = d.kind == ParamKind::Choice ? static_cast<double>(d.choices.size() - 1) : d.max;
    if (!std::isfinite(v)) v = d.def;
    st.params[static_cast<size_t>(k)] = static_cast<float>(std::clamp(v, d.min, hi));
    ++rep.mapped;
  }
  static const Json* plain(const Json* mod) {
    if (!mod) return nullptr;
    const Json* p = mod->find("plainParams");
    return p && p->type == Json::Type::Object ? p : nullptr;
  }
  // Values read from the document (coverage: what is left over is reported as unread).
  std::set<const Json*> used;
  bool num(const Json* pp, const char* key, double& v) {
    if (!pp) return false;
    const Json* j = pp->find(key);
    if (!j || j->type != Json::Type::Number) return false;
    used.insert(j);
    v = j->number;
    return true;
  }
  bool text(const Json* pp, const char* key, std::string& v) {
    if (!pp) return false;
    const Json* j = pp->find(key);
    if (!j || j->type != Json::Type::String) return false;
    used.insert(j);
    v = j->str;
    return true;
  }
  void apply(const Json* pp, const KeyMap* maps, size_t n, int base) {
    if (!pp) return;
    for (size_t i = 0; i < n; ++i) {
      double v;
      if (num(pp, maps[i].key, v)) set(base + maps[i].param, conv(maps[i].c, v));
    }
  }

  // Every stored module value the mapping did not read ("Module.kParamKey", digits dropped).
  void unread(const Json& o, const std::string& mod) {
    if (o.type != Json::Type::Object) {
      if (o.type == Json::Type::Array)
        for (const auto& x : o.array) unread(x, mod);
      return;
    }
    for (const auto& kv : o.object) {
      if (kv.first == "plainParams" && kv.second.type == Json::Type::Object) {
        for (const auto& p : kv.second.object)
          if (!used.count(&p.second)) {
            const std::string k = mod + "." + p.first;
            if (std::find(rep.unread.begin(), rep.unread.end(), k) == rep.unread.end()) rep.unread.push_back(k);
          }
        continue;
      }
      if (kv.second.type != Json::Type::Object && kv.second.type != Json::Type::Array) continue;
      std::string m = kv.first;
      while (!m.empty() && std::isdigit(static_cast<unsigned char>(m.back()))) m.pop_back();
      unread(kv.second, kv.second.type == Json::Type::Object && !m.empty() && m != "FX" ? m : mod);
    }
  }

  // ---------------------------------------------------------------- files
  std::string resolve(const std::string& rel, std::initializer_list<const char*> subdirs) {
    if (rel.empty() || !files) return "";
    std::string r = rel;
    std::replace(r.begin(), r.end(), '\\', '/');
    while (!r.empty() && r[0] == '/') r.erase(0, 1);
    std::error_code ec;
    if (fs::path(rel).is_absolute() && fs::exists(rel, ec)) return rel;
    // next to the preset (a pack's own Tables / Samples folders): direct lookups, no folder walk
    const fs::path name = fs::u8path(r).filename();
    for (const auto& d : near) {
      const fs::path base = fs::u8path(d);
      if (fs::path p = base / fs::u8path(r); fs::is_regular_file(p, ec)) return p.u8string();
      for (const char* sub : subdirs)
        for (const char* dir : {sub, "Wavetables", "Samples", "Noises", "Impulses", "Multisamples"}) {
          if (!*dir) continue;
          if (fs::path p = base / dir / fs::u8path(r); fs::is_regular_file(p, ec)) return p.u8string();
          if (fs::path p = base / dir / name; fs::is_regular_file(p, ec)) return p.u8string();
        }
      if (fs::path p = base / name; fs::is_regular_file(p, ec)) return p.u8string();
    }
    for (const auto& root : roots)
      for (const char* sub : subdirs) {
        fs::path p = fs::path(root) / sub / r;
        if (fs::exists(p, ec)) return p.string();
      }
    // by file name below the sub folders (bounded walk)
    for (const auto& root : roots)
      for (const char* sub : subdirs) {
        fs::path base = fs::path(root) / sub;
        if (!fs::is_directory(base, ec)) continue;
        int visited = 0;
        for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && visited < 40000; it.increment(ec), ++visited) {
          if (ec) break;
          if (it->is_regular_file(ec) && it->path().filename() == name) return it->path().string();
        }
      }
    return "";
  }
  // A microtuning the preset carries (tuningData: the bytes of a .tun file).
  void tuning(const Json& root) {
    const Json* td = root.find("tuningData");
    if (!td || td->type != Json::Type::Array || td->array.empty() || td->array.size() > (size_t(1) << 20)) return;
    std::string text;
    text.reserve(td->array.size());
    for (const auto& b : td->array)
      if (b.type == Json::Type::Number && b.number >= 1 && b.number < 256) text += static_cast<char>(static_cast<unsigned char>(b.number));
    if (text.find('=') == std::string::npos) return;
    used.insert(td);
    m.tuning = text;
    m.tuningName = root.text("tuningName", "Preset tuning");
  }

  // ---------------------------------------------------------------- embedded audio
  // {data: [channels], sampleRate} -> a media reference ("" when there is no audio)
  std::string embeddedAudio(const Json& ea, const std::string& name) {
    const Json* data = ea.find("data");
    std::vector<std::vector<float>> ch;
    if (!data || !serum::channelsOf(*data, ch) || ch[0].empty()) return "";
    return addMedia(m, name, static_cast<int>(std::lround(ea.num("sampleRate", 44100))), ch);
  }
  static std::string slashPath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && (p.back() == ' ' || p.back() == '\r' || p.back() == '\t')) p.pop_back();
    return p;
  }
  static std::string baseName(const std::string& p) {
    const std::string n = slashPath(p);
    const size_t sl = n.find_last_of('/');
    return sl == std::string::npos ? n : n.substr(sl + 1);
  }
  // embedded_sfz + embedded_audio {"<path>": {data, sampleRate, loopStart, loopEnd}} -> a media map reference
  std::string embeddedMap(const Json& sub, const std::string& rel) {
    const Json* ea = sub.find("embedded_audio");
    if (!ea || ea->type != Json::Type::Object || ea->object.empty()) return "";
    struct Member {
      std::string key, ref;
      double loopStart = 0, loopEnd = 0;
    };
    std::vector<Member> members;
    for (const auto& kv : ea->object) {
      if (kv.second.type != Json::Type::Object) continue;
      Member mb;
      mb.key = kv.first;
      mb.ref = embeddedAudio(kv.second, baseName(kv.first));
      mb.loopStart = kv.second.num("loopStart", 0);
      mb.loopEnd = kv.second.num("loopEnd", 0);
      if (!mb.ref.empty()) members.push_back(std::move(mb));
    }
    if (members.empty()) return "";
    std::string sfz = sub.text("embedded_sfz");
    if (sfz.empty())  // samples without a map: each one over the whole keyboard
      for (const auto& mb : members) sfz += "<region> sample=" + mb.key + "\n";
    // every sample= value points at its embedded member (parsed like the map reader does:
    // the value runs to the end of the line or to the next opcode)
    std::string out;
    size_t i = 0;
    for (size_t at = sfz.find("sample=", i); at != std::string::npos; at = sfz.find("sample=", i)) {
      const size_t vs = at + 7;
      size_t ve = sfz.find_first_of("<\n\r", vs);
      if (ve == std::string::npos) ve = sfz.size();
      if (const size_t eq2 = sfz.find('=', vs); eq2 != std::string::npos && eq2 < ve)
        if (const size_t sp = sfz.rfind(' ', eq2); sp != std::string::npos && sp > vs) ve = sp;
      const std::string val = slashPath(sfz.substr(vs, ve - vs));
      const Member* hit = nullptr;
      for (const auto& mb : members)
        if (slashPath(mb.key) == val) hit = &mb;
      if (!hit)
        for (const auto& mb : members)
          if (baseName(mb.key) == baseName(val)) hit = &mb;
      out += sfz.substr(i, vs - i);
      if (hit) {
        out += hit->ref;
        const size_t eol = sfz.find_first_of("\n\r", vs);
        const std::string line = sfz.substr(at, (eol == std::string::npos ? sfz.size() : eol) - at);
        if (hit->loopEnd > hit->loopStart && line.find("loop_") == std::string::npos)
          out += " loop_start=" + std::to_string(static_cast<int64_t>(hit->loopStart)) + " loop_end=" +
                 std::to_string(static_cast<int64_t>(hit->loopEnd));
      } else {
        out += sfz.substr(vs, ve - vs);
      }
      i = ve;
    }
    out += sfz.substr(i);
    // the members sit next to the extracted map: a folder prefix would point elsewhere
    for (size_t p = out.find("default_path="); p != std::string::npos; p = out.find("default_path=", p)) {
      const size_t e = out.find_first_of("<\n\r", p);
      out.erase(p, (e == std::string::npos ? out.size() : e) - p);
    }
    return addMediaMap(m, rel.empty() ? std::string("Multisample") : baseName(rel), out);
  }

  // embedded_sfz without embedded audio: the source plays the map text the preset carries and finds its
  // samples relative to where the map sat when the preset was saved (<content>/Multisamples/<sfzPathRelative>),
  // even when the map file itself has moved since (J-203a: measured, a factory drum kit whose stored map path
  // is stale plays; without the stored path it is silent). Samples are written as absolute paths.
  std::string referencedMap(const Json& sub, const std::string& rel) {
    const std::string sfz = sub.text("embedded_sfz");
    if (sfz.empty() || !files) return "";
    std::vector<fs::path> bases;
    const fs::path relDir = fs::u8path(slashPath(rel)).relative_path().parent_path();
    for (const auto& d : near) bases.push_back(fs::u8path(d) / "Multisamples" / relDir), bases.push_back(fs::u8path(d) / relDir);
    for (const auto& r : roots) bases.push_back(fs::u8path(r) / "Multisamples" / relDir);
    if (const std::string found = resolve(rel, {"Multisamples", "Multisamples/Factory", "Multisamples/User", ""}); !found.empty())
      bases.push_back(fs::u8path(found).parent_path());
    std::string dp;
    if (const size_t p = sfz.find("default_path="); p != std::string::npos) {
      const size_t e = sfz.find_first_of("<\n\r", p);
      dp = slashPath(sfz.substr(p + 13, (e == std::string::npos ? sfz.size() : e) - p - 13));
      if (!dp.empty() && dp.back() != '/') dp += '/';
    }
    std::error_code ec;
    auto find = [&](const std::string& val) -> std::string {
      const fs::path v = fs::u8path(val);
      if (v.is_absolute() && fs::is_regular_file(v, ec)) return v.generic_u8string();
      for (const auto& b : bases)
        for (const fs::path& c : {b / fs::u8path(dp + val), b / v})
          if (fs::is_regular_file(c, ec)) return c.lexically_normal().generic_u8string();
      return byName(baseName(val));
    };
    std::string out;
    size_t i = 0;
    int found = 0, lost = 0;
    for (size_t at = sfz.find("sample=", i); at != std::string::npos; at = sfz.find("sample=", i)) {
      const size_t vs = at + 7;
      size_t ve = sfz.find_first_of("<\n\r", vs);
      if (ve == std::string::npos) ve = sfz.size();
      if (const size_t eq2 = sfz.find('=', vs); eq2 != std::string::npos && eq2 < ve)
        if (const size_t sp = sfz.rfind(' ', eq2); sp != std::string::npos && sp > vs) ve = sp;
      const std::string val = slashPath(sfz.substr(vs, ve - vs));
      const std::string hit = find(val);
      out += sfz.substr(i, vs - i);
      if (!hit.empty()) out += hit, ++found;
      else out += sfz.substr(vs, ve - vs), ++lost, missing(val);
      i = ve;
    }
    out += sfz.substr(i);
    if (!found) return "";
    if (lost) note("Multisample: " + std::to_string(lost) + " sample file(s) not found");
    for (size_t p = out.find("default_path="); p != std::string::npos; p = out.find("default_path=", p)) {
      const size_t e = out.find_first_of("<\n\r", p);
      out.erase(p, (e == std::string::npos ? out.size() : e) - p);
    }
    return addMediaMap(m, rel.empty() ? std::string("Multisample") : baseName(rel), out);
  }
  // a sample file by name below the content folders (one bounded walk, indexed)
  std::map<std::string, std::string> nameIndex;
  bool indexed = false;
  std::string byName(const std::string& name) {
    if (!indexed) {
      indexed = true;
      std::error_code ec;
      for (const auto& root : roots)
        for (const char* sub : {"Samples", "Multisamples"}) {
          int visited = 0;
          for (auto it = fs::recursive_directory_iterator(fs::u8path(root) / sub, fs::directory_options::skip_permission_denied, ec);
               it != fs::recursive_directory_iterator() && visited < 60000; it.increment(ec), ++visited) {
            if (ec) break;
            if (it->is_regular_file(ec)) nameIndex.emplace(it->path().filename().u8string(), it->path().generic_u8string());
          }
        }
    }
    auto it = nameIndex.find(name);
    return it == nameIndex.end() ? std::string() : it->second;
  }

  void missing(const std::string& what) {
    if (std::find(rep.missingFiles.begin(), rep.missingFiles.end(), what) == rep.missingFiles.end()) rep.missingFiles.push_back(what);
  }

  // ---------------------------------------------------------------- curves (y is stored top-down)
  static Curve curve(const Json* c) {
    Curve out;
    if (!c || c->type != Json::Type::Object) return out;
    const Json* xs = c->find("xVals");
    const Json* ys = c->find("yVals");
    const Json* cs = c->find("curveVals");
    if (!xs || !ys || xs->type != Json::Type::Array || ys->type != Json::Type::Array) return out;
    const int np = static_cast<int>(c->num("numPoints", static_cast<double>(xs->array.size()) - 1)) + 1;
    const int n = std::min({np, static_cast<int>(xs->array.size()), static_cast<int>(ys->array.size()), 128});
    for (int i = 0; i < n; ++i) {
      CurvePt p;
      p.x = static_cast<float>(std::clamp(xs->array[static_cast<size_t>(i)].number, 0.0, 1.0));
      p.y = static_cast<float>(std::clamp(1.0 - ys->array[static_cast<size_t>(i)].number, 0.0, 1.0));
      if (i > 0 && cs && cs->type == Json::Type::Array && static_cast<size_t>(i - 1) < cs->array.size())
        p.c = static_cast<float>(std::clamp((cs->array[static_cast<size_t>(i - 1)].number - 0.5) * 2.0, -1.0, 1.0));
      if (!out.empty() && p.x < out.back().x) p.x = out.back().x;
      out.push_back(p);
    }
    return out;
  }

  // ---------------------------------------------------------------- modules
  void oscillators(const Json& o) {
    set(tailP(T_UniMix), 1.0);  // the source's unison voice mix (J-201, measured)
    set(tailP(T_OscLaw), 1.0);
    set(tailP(T_ModLaw), 1.0);  // matrix amounts on the source's knob curves (J-203g, measured)  // frame spread, frame DC and warp depths as the source plays them (J-203g, measured)
    for (int i = 0; i < kOscs; ++i) {
      const Json* osc = o.find("Oscillator" + std::to_string(i));
      if (!osc) continue;
      const Json* pp = plain(osc);
      set(oscP(i, O_UniDetune), conv(Detune, 0.25));  // the source's default detune (not stored)
      apply(pp, kOscMain, sizeof(kOscMain) / sizeof(kOscMain[0]), oscP(i, 0));
      std::string s;
      int type = OscWavetable;
      if (text(pp, "kParamType", s)) {
        static const char* const types[] = {"kOsc_WT", "kOsc_MultiSample", "kOsc_Sample", "kOsc_Granular", "kOsc_Spectral"};
        type = std::max(0, indexOf(types, 5, s));
      }
      oscType[static_cast<size_t>(i)] = type;
      set(oscP(i, O_Type), type);
      int pitchMode = 0;
      if (text(pp, "kParamPitchMode", s)) {
        static const char* const pm[] = {"Semitones", "Steps", "Harmonics", "Ratio"};
        pitchMode = std::max(0, indexOf(pm, 4, s));
        set(oscP(i, O_PitchMode), pitchMode);
      }
      // J-201: the source only adds its Hz offset in Ratio mode (measured: a 100 Hz offset moves a
      // key-36 tone from 65.4 to 165.4 Hz in Ratio mode and leaves it at 65.4 Hz in the other modes).
      if (pitchMode != 3) set(oscP(i, O_HzOffset), 0.0);
      if (text(pp, "kParamUnisonStack", s)) {
        static const char* const sk[] = {"kNoUnisonStack", "kOctave1", "kOctave2", "kOctave3", "kOctaveFifth1",
                                         "kOctaveFifth2", "kOctaveFifth3", "kCenter12", "kCenter24"};
        set(oscP(i, O_UniStack), std::max(0, indexOf(sk, 9, s)));
      }
      if (text(pp, "kParamDetuneMode", s)) {
        static const char* const dm[] = {"kDetuneLinear", "kDetuneSuper", "kDetuneExp", "kDetuneInv", "kDetuneRandom"};
        set(oscP(i, O_DetuneMode), std::max(0, indexOf(dm, 5, s)));
      }
      if (text(pp, "kParamLoopMode", s)) {
        static const char* const lm[] = {"kNone", "kForward", "kReverse", "kPingPong", "kTailed"};
        set(oscP(i, O_LoopMode), std::max(0, indexOf(lm, 5, s)));
      }
      double v;
      if (num(pp, "kParamLoopEndsAtRelease", v) && v >= 0.5) note("Sample loop that ends at note release: plays as a normal loop");
      if (num(pp, "kParamScanBPMRate", v)) note("Tempo-synced sample scan rate: approximated by the free scan rate");
      if (const Json* sm = osc->find("sliceMarkers"); sm && sm->type == Json::Type::Array)
        for (const auto& x : sm->array)
          if (x.type == Json::Type::Number) m.osc[static_cast<size_t>(i)].slices.push_back(static_cast<float>(std::clamp(x.number, 0.0, 1.0)));
      set(oscP(i, O_PhaseMemory), 2);  // Serum's default: all voices share free-running phases
      // sub-objects of the active type
      static const char* const subNames[] = {"WTOsc", "MultiSampleOsc", "SampleOsc", "GranularOsc", "SpectralOsc"};
      const Json* sub = osc->find(subNames[type] + std::to_string(i));
      const Json* sp = plain(sub);
      for (int k = 0; k < 5; ++k)  // the other source types' settings are not heard
        if (k != type)
          if (const Json* other = plain(osc->find(subNames[k] + std::to_string(i))))
            for (const auto& kv : other->object) used.insert(&kv.second);
      set(oscP(i, O_Smooth), 0.0);  // frame blending is off unless the preset turns it on (J-203g, measured)
      // a wavetable starts at 180 degrees unless the preset says otherwise (J-203i, measured: single
      // cycles of the start-up saw move by half a cycle between phase 0 and the default)
      if (type == OscWavetable) set(oscP(i, O_Phase), 0.5);
      apply(sp, kOscSub, sizeof(kOscSub) / sizeof(kOscSub[0]), oscP(i, 0));
      for (int w = 0; w < 2; ++w) {
        if (text(sp, w == 0 ? "kParamWarpMenu" : "kParamWarpMenu2", s)) {
          const int wi = warpIndex(s, type);
          if (wi < 0) note("Warp mode " + s + " is not available: off");
          set(oscP(i, w == 0 ? O_Warp1Mode : O_Warp2Mode), std::max(0, wi));
        }
      }
      if (text(sp, "kParamPhaseMemory", s)) {
        static const char* const pmm[] = {"kAll", "kContiguous", "kPerVoice"};
        const int k = indexOf(pmm, 3, s);
        set(oscP(i, O_PhaseMemory), k == 0 ? 2 : k == 1 ? 1 : 0);
      }
      if (!sub) continue;
      if (type == OscWavetable) wavetable(i, *sub, sp);
      else sampleSource(i, type, *sub, sp);
      // remap curves / flex: the wavetable's drawable warp curve
      if (const Json* fl = sub->find("flex")) {
        Curve c = curve(fl);
        if (c.size() >= 2) m.osc[static_cast<size_t>(i)].remap[0] = c;
      }
    }
  }
  void wavetable(int i, const Json& sub, const Json* sp) {
    OscData& d = m.osc[static_cast<size_t>(i)];
    const double total = sub.num("numFrames", 2048);
    int frames = std::max(1, static_cast<int>(std::lround(total / kFrameSize)));
    std::string rel = sub.text("relativePathToWT");
    // The table the preset carries is what it plays (its numFrames can be stale);
    // the referenced file is only looked up when nothing is embedded.
    std::vector<float> emb;
    const Json* ej = sub.find("embeddedWTData");
    const bool hasEmb = ej && serum::floatsOf(*ej, emb) && emb.size() >= static_cast<size_t>(kFrameSize);
    const bool builtIn = rel.empty() && !ej;  // the synth's own start-up table
    if (builtIn) rel = "S2 Tables/Default Shapes.wav";
    const std::string path = hasEmb ? std::string() : resolve(rel, {"Tables", ""});
    if (hasEmb) {
      const size_t nf = std::min<size_t>(256, emb.size() / kFrameSize);
      frames = static_cast<int>(nf);
      d.frames.assign(nf, std::vector<float>(kFrameSize, 0.f));
      for (size_t f = 0; f < nf; ++f) std::copy_n(emb.begin() + static_cast<std::ptrdiff_t>(f * kFrameSize), kFrameSize, d.frames[f].begin());
      if (emb.size() / kFrameSize > 256) note("Wavetables longer than 256 frames are shortened to 256");
      d.table = "custom";
      d.tableName = sub.text("tableDisplayName", rel.empty() ? std::string("Embedded") : fs::path(rel).stem().string());
      set(oscP(i, O_Table), 0);
    } else if (!path.empty()) {
      d.table = "file:" + path;
      set(oscP(i, O_Table), 0);
    } else if (builtIn) {
      set(oscP(i, O_Table), 1);  // Basic Shapes: saw at the start, like the start-up table
      set(oscP(i, O_WtPos), 0.0);
    } else {
      if (!rel.empty()) missing(rel);
      note("Wavetable not found: a built-in table plays instead");
      set(oscP(i, O_Table), 1);
    }
    oscFrames[static_cast<size_t>(i)] = frames;
    double pos;
    if (num(sp, "kParamTablePos", pos)) set(oscP(i, O_WtPos), (pos - 1.0) / 255.0);  // 1..256 spans the table, whatever its length
  }
  void sampleSource(int i, int type, const Json& sub, const Json* sp) {
    OscData& d = m.osc[static_cast<size_t>(i)];
    std::string s;
    if (type == OscMultisample) {
      const std::string rel = sub.text("sfzPathRelative", sub.text("sfzPath"));
      std::string emb = files ? embeddedMap(sub, rel) : std::string();
      if (emb.empty() && files) emb = referencedMap(sub, rel);
      const std::string path = emb.empty() ? resolve(rel, {"Multisamples", "Multisamples/Factory", "Multisamples/User", ""}) : emb;
      if (!path.empty()) d.sfz = path;
      else if (!rel.empty()) missing(rel);
      return;
    }
    const std::string rel = sub.text("samplePathRelative", sub.text("samplePath"));
    std::string path;
    if (const Json* ea = sub.find("embedded_audio"); ea && files)
      path = embeddedAudio(*ea, ea->text("table_name", fs::path(rel).filename().string()));
    if (path.empty()) path = resolve(rel, {"Samples", "Samples/Factory", "Samples/Factory Non-Tonal", "Samples/User", ""});
    if (!path.empty()) d.sample = path;
    else if (!rel.empty()) missing(rel);
    if (type == OscGranular) {
      double v;
      if (num(sp, "kParamDensity", v)) set(oscP(i, O_GDensity), std::log(std::max(1.0, v)) / std::log(200.0));
      if (num(sp, "kParamGrainLength", v)) set(oscP(i, O_GLength), std::clamp(std::log(std::max(2.0, v * 1000.0) / 2.0) / std::log(500.0), 0.0, 1.0));
      if (num(sp, "kParamRandomPitch", v)) set(oscP(i, O_GRndPitch), v / 12.0);
      if (text(sp, "kParamDensityMode", s)) {
        static const char* const k[] = {"kDensityFree", "kDensityBPM", "kDensityGrains"};
        set(oscP(i, O_GDensMode), std::max(0, indexOf(k, 3, s)));
      }
      if (text(sp, "kParamLengthMode", s)) {
        static const char* const k[] = {"kLengthFree", "kLengthBPM", "kLengthPercent"};
        set(oscP(i, O_GLenMode), std::max(0, indexOf(k, 3, s)));
      }
      if (text(sp, "kParamWindowShape", s)) {
        static const char* const k[] = {"kWindowHann", "kWindowWelch", "kWindowGaussian", "kWindowBlackmanHarris", "kWindowSinc",
                                        "kWindowTukey", "kWindowTriangle", "kWindowTrapezoid", "kWindowExpDec", "kWindowExpDecRev"};
        set(oscP(i, O_GWinShape), std::max(0, indexOf(k, 10, s)));
      }
      if (text(sp, "kParamUnisonTrigPattern", s)) {
        static const char* const k[] = {"kTogether", "kEven", "kExponential", "kRandom"};
        set(oscP(i, O_GTrig), std::max(0, indexOf(k, 4, s)));
      }
    }
    if (type == OscSpectral) {
      double v;
      if (num(sp, "kParamFreqLo", v)) set(oscP(i, O_SLo), std::clamp(cutoffNorm(std::max(8.0, v)), 0.0, 1.0));
      if (num(sp, "kParamFreqHi", v)) set(oscP(i, O_SHi), std::clamp(cutoffNorm(std::max(8.0, v)), 0.0, 1.0));
    }
  }
  void noiseSub(const Json& o) {
    if (const Json* n = o.find("Oscillator3")) {
      const Json* pp = plain(n);
      double v;
      if (num(pp, "kParamEnable", v)) set(noiseP(N_Enable), v);
      // same curve as the oscillators (also at the default level, 0.25 on the knob, which a
      // preset does not store); kNoiseLevelTrim matches sample noises (J-201: measured with
      // embedded sines from 30 Hz to 10 kHz, the earlier 1.122 played 2 dB too loud)
      double lv = 0.0625;
      num(pp, "kParamVolume", lv);
      set(noiseP(N_Level), std::min(1.0, std::sqrt(std::max(0.0, lv)) * kNoiseLevelTrim));
      if (num(pp, "kParamPan", v)) set(noiseP(N_Pan), v / 50.0);
      double track = 0;
      num(pp, "kParamPitchTrack", track);
      set(noiseP(N_PitchTrack), track);
      const Json* sub = n->find("NoiseOsc3");
      const Json* sp = plain(sub);
      // The noise "pitch" knob (stored as kParamColor, 0..1, 0.5 = the sample's own
      // rate) is a playback-rate control, measured on the source (J-201): without key
      // tracking +96 semitones per unit above the centre and 384 below it (the lower
      // half drops to near-silent rumble); with key tracking a plain 128 semitone
      // span, with the sample's original rate at MIDI key 72 for the usual
      // detuneFactor (0.4758...), shifting by 12 log2(detuneFactor / 0.4758...).
      // Fine is +-1 semitone (+-0.96 untracked).
      double x = 0.5, fine = 0;
      num(sp, "kParamColor", x);
      num(sp, "kParamFine", fine);
      double semis;
      if (track >= 0.5) {
        const double df = sub ? sub->num("detuneFactor", kNoiseDetuneRef) : kNoiseDetuneRef;
        semis = 12.0 * std::log2(std::max(1e-3, df) / kNoiseDetuneRef) - 12.0 + 128.0 * (x - 0.5);
        noisePitchModScale = 1.0;
      } else {
        semis = (x >= 0.5 ? 96.0 : 384.0) * (x - 0.5);
        noisePitchModScale = x >= 0.5 ? 0.75 : 3.0;
      }
      if (semis < -64.0) note("Noise pitch below -64 semitones: played at -64");
      set(noiseP(N_Pitch), semis);
      set(noiseP(N_Fine), fine * (track >= 0.5 ? 1.0 : 0.96));
      set(noiseP(N_Color), 0.0);
      if (num(sp, "kParamInitialPhase", v)) set(noiseP(N_Phase), v / 100.0);
      if (num(sp, "kParamRandomPhase", v)) set(noiseP(N_RandPhase), v / 100.0);
      if (num(sp, "kParamOneShot", v)) set(noiseP(N_OneShot), v);
      std::string type = "Sample";
      text(sp, "kParamNoiseType", type);
      const auto& names = noiseNames();
      auto byName = [&](const char* nm) {
        for (size_t k = 0; k < names.size(); ++k)
          if (names[k] == nm) return static_cast<int>(k);
        return 1;
      };
      // J-201: on the built-in noises the same knob is a tone filter instead of a pitch (see Noise
      // Tone in the engine), and they play quieter than Plasma's generated tables: level trims
      // measured on held notes at key 60 (white -3.9 dB, pink -2.4 dB, brown -1.4 dB).
      auto builtin = [&](const char* nm, double trimDb, bool tone) {
        set(noiseP(N_Type), byName(nm));
        set(noiseP(N_Pitch), 0.0);
        set(noiseP(N_Fine), 0.0);
        set(noiseP(N_PitchTrack), 0.0);
        if (tone) set(tailP(T_NoiseTone), std::clamp(2.0 * (x - 0.5), -1.0, 1.0));
        set(noiseP(N_Level), std::min(1.0, std::sqrt(std::max(0.0, lv)) * std::pow(10.0, trimDb / 40.0)));
        builtinNoise = tone;
      };
      if (type == "White") builtin("White", -3.9, true);
      else if (type == "Pink") builtin("Pink", -2.4, true);
      else if (type == "Brown") builtin("Brown", -1.4, true);
      else if (type == "Geiger") {
        // a different click texture than the source's; level matched on held notes (+10 dB),
        // its knob (density on the source) is left at the centre
        builtin("Geiger", 10.0, false);
        geigerNoise = true;
        note("Geiger noise: approximated (click texture and density differ)");
      }
      else if (sub) {
        const std::string rel = sub->text("relativePathToNoiseSample", sub->text("pathToNoiseSample"));
        std::string path;
        if (const Json* e = sub->find("embeddedNoiseData"); e && files) {
          std::vector<std::vector<float>> ch;
          // the noise carries no rate of its own: the synth's noises are 44.1 kHz
          if (serum::channelsOf(*e, ch) && !ch[0].empty())
            path = addMedia(m, rel.empty() ? std::string("Noise") : fs::path(rel).filename().string(), 44100, ch);
        }
        if (path.empty()) path = resolve(rel, {"Samples/Factory Non-Tonal/Noises", "Samples", "Noises", ""});
        if (!path.empty()) {
          m.noiseFile = path;
          set(noiseP(N_Type), 0);
        } else {
          if (!rel.empty()) missing(rel);
          note("Noise sample not found: a built-in noise plays instead");
          set(noiseP(N_Type), 1);
        }
      }
    }
    {
      // same curve as the oscillators; Plasma's sub sine sits 3.6 dB higher at equal settings
      // (also at the default level, which a preset does not store)
      double v = 0.75;  // the source's default (stored as knob squared)
      num(plain(o.find("Oscillator4")), "kParamVolume", v);
      set(subP(S_Level), std::sqrt(std::max(0.0, v)) * 0.8128);
    }
    if (const Json* s = o.find("Oscillator4")) {
      const Json* pp = plain(s);
      double v;
      if (num(pp, "kParamEnable", v)) set(subP(S_Enable), v);
      if (num(pp, "kParamPan", v)) set(subP(S_Pan), v / 50.0);
      if (num(pp, "kParamOctave", v)) set(subP(S_Octave), v);
      if (num(pp, "kParamCoarsePit", v)) set(subP(S_Coarse), v);
      if (num(pp, "kParamPitchTrack", v)) set(subP(S_PitchTrack), v);
      const Json* sp = plain(s->find("SubOsc4"));
      std::string shape;
      if (text(sp, "kParamShape", shape)) {
        static const char* const k[] = {"kSine", "kRoundRect", "kTriangle", "kSaw", "kSquare", "kPulse"};
        set(subP(S_Shape), std::max(0, indexOf(k, 6, shape)));
      }
      if (num(sp, "kParamInitialPhase", v)) set(subP(S_Phase), v / 360.0);
      if (num(sp, "kParamContiguousPhase", v)) set(subP(S_ContPhase), v);
    }
  }
  void filters(const Json& o) {
    for (int f = 0; f < kFilters; ++f) {
      const Json* pp = plain(o.find("VoiceFilter" + std::to_string(f)));
      apply(pp, kFiltMap, sizeof(kFiltMap) / sizeof(kFiltMap[0]), filtP(f, 0));
      std::string s;
      if (text(pp, "kParamType", s)) {
        const int t = indexOf(kFilterKeys, kFilterKeyCount, s);
        if (t < 0) note("Filter type " + s + " is not available");
        set(filtP(f, F_Type), t < 0 ? 1 : t);
      }
      double v;
      if (num(pp, "kParamPad", v)) set(tailP(T_Pad1 + f), v >= 0.5 ? 1.0 : 0.0);
      // Drive: the source's filter keeps the driven level (drive 0.5 is about +14 dB): the
      // Boost drive mode (set below) follows its measured gain and clip law.
    }
    set(tailP(T_DriveMode), 1.0);
    set(tailP(T_KeyCentre), 1.0);  // filter key tracking pivots on key 32 (J-203b: measured, slope 1)
    set(tailP(T_MsLaw), 1.0);      // multisample velocity, envelope and level as the source plays them (J-203a)
    for (int r = 0; r < kRoutes; ++r) {
      const Json* pp = plain(o.find("RoutingSlot" + std::to_string(r)));
      apply(pp, kRouteMap, sizeof(kRouteMap) / sizeof(kRouteMap[0]), routeP(r, 0));
      std::string s;
      if (text(pp, "kParamRoutingDest", s)) {
        static const char* const k[] = {"kRoutingDestFilter", "kRoutingDestMaster", "kRoutingDestDirect", "kRoutingDestNone"};
        set(routeP(r, R_Dest), std::max(0, indexOf(k, 4, s)));
      }
    }
  }
  void envLfoMacros(const Json& o) {
    // a preset stores only the values that differ from the source's own defaults, which are not
    // Plasma's (J-203a, read from the hosted source's parameter defaults and checked by a save
    // round trip): every curve 60, except the attack curve of envelopes 2-4 at 40; their attack is
    // 0.5 ms (envelope 1: 1 ms). Its curves bend on its own law (T_EnvLaw).
    set(tailP(T_EnvLaw), 1.0);
    set(tailP(T_LfoLaw), 1.0);  // LFO rise, delay and smoothing as the source plays them (J-203i)
    for (int e = 0; e < kEnvs; ++e) {
      set(envP(e, E_CurveA), e == 0 ? 60.0 : 40.0), set(envP(e, E_CurveD), 60.0), set(envP(e, E_CurveR), 60.0);
      if (e > 0) set(envP(e, E_Attack), envTimeInv(0.0005));
      apply(plain(o.find("Env" + std::to_string(e))), kEnvMap, sizeof(kEnvMap) / sizeof(kEnvMap[0]), envP(e, 0));
    }
    for (int l = 0; l < kLfos; ++l) {
      const Json* lfo = o.find("LFO" + std::to_string(l));
      if (!lfo) continue;
      const Json* pp = plain(lfo);
      apply(pp, kLfoMap, sizeof(kLfoMap) / sizeof(kLfoMap[0]), lfoP(l, 0));
      double r10 = 0, rate = 0;
      if (num(pp, "kParamRate10x", r10) && r10 >= 0.5 && num(pp, "kParamRate", rate)) set(lfoP(l, L_Rate), lfoHzInv(std::clamp(rate, 0.0, 1000.0), true));
      std::string s;
      if (text(pp, "kParamMode", s)) {
        static const char* const k[] = {"Free", "Retrig", "Envelope"};
        set(lfoP(l, L_Mode), std::max(0, indexOf(k, 3, s)));
      }
      if (text(pp, "kParamType", s)) {
        static const char* const k[] = {"Curve", "Path", "Lorenz", "Rossler", "RandomSH"};
        set(lfoP(l, L_Type), std::max(0, indexOf(k, 5, s)));
      }
      double v, bs = 1;
      num(pp, "kParamBeatSync", bs);
      for (const char* key : {"kParamRise", "kParamDelay"})
        if (num(pp, key, v)) {
          // J-203i (measured on the hosted source): on a free LFO the value is seconds; on a synced
          // one it snaps to a power-of-two beat count, about 2^round(3.33 (v - 2.1)); the measured
          // steps (1/8 -> 1/4 beat at 1.32, then 1.65, 1.95, 2.22, 2.55, 2.85, 8 -> 16 at 3.05) are
          // used where they were measured. Plasma's imported LFO law reads 16 x^2.
          static const double kStep[] = {1.32, 1.65, 1.95, 2.22, 2.55, 2.85, 3.05};
          double t = 0;
          if (v > 1e-6 && bs < 0.5) t = v;
          else if (v > 1e-6) {
            int k = 0;
            while (k < 7 && v >= kStep[k]) ++k;
            t = k == 0 ? std::min(0.125, std::exp2(std::round(3.33 * (v - 2.1)))) : k == 7 ? std::max(16.0, std::exp2(std::round(3.33 * (v - 2.1)))) : std::exp2(k - 3);
          }
          set(lfoP(l, key[6] == 'R' ? L_Rise : L_Delay), std::clamp(std::sqrt(t / 16.0), 0.0, 1.0));
        }
      if (const Json* cd = lfo->find("curveData")) {
        Curve c = curve(cd);
        if (c.size() >= 2) {
          LfoData& d = m.lfo[static_cast<size_t>(l)];
          d.pts = c;
          const int lb = static_cast<int>(cd->num("loopbackPointNum", -1));
          d.loop = lb >= 0 && lb < static_cast<int>(c.size()) ? lb : -1;
          d.name = lfo->text("curveDisplayName", "Imported");
          set(lfoP(l, L_Shape), 0);
        }
      }
      if (const Json* pd = lfo->find("pathData")) {
        Curve c = curve(pd);
        if (c.size() >= 2) {
          LfoData& d = m.lfo[static_cast<size_t>(l)];
          d.path.clear();
          d.pathX.clear();
          for (size_t i = 0; i < c.size(); ++i) {
            d.path.push_back({static_cast<float>(i) / static_cast<float>(c.size() - 1), c[i].y, c[i].c});
            d.pathX.push_back(c[i].x);
          }
          note("Path LFO: points are traversed at an even speed");
        }
      }
    }
    for (int k = 0; k < kMacros; ++k) {
      const Json* mc = o.find("Macro" + std::to_string(k));
      double v;
      if (num(plain(mc), "kParamValue", v)) set(macroP(k), v / 100.0);
      if (mc) m.macroName[static_cast<size_t>(k)] = mc->text("name");
    }
    for (int b = 0; b < kLfoBusses; ++b) {
      double v;
      if (num(plain(o.find("LFOPointModBus" + std::to_string(b))), "kParamValue", v)) set(busP(b), v);
    }
    if (const Json* sc = o.find("scalars")) {
      Curve v = curve(sc->find("velo")), n = curve(sc->find("note"));
      if (v.size() >= 2) m.velo = v;
      if (n.size() >= 2) m.note = n;
    }
  }
  void global(const Json& o) {
    const Json* pp = plain(o.find("Global0"));
    double v;
    std::string s;
    if (num(pp, "kParamMasterVolume", v)) set(G_MasterVol, std::sqrt(std::max(0.0, v)));
    if (num(pp, "kParamMasterTuning", v)) {
      // J-201: the source stores its main tuning as 0..1 = -64..+64 semitones (measured against the
      // hosted source: 0.25 -> -32 st, 0.75 -> +32 st, 1 -> +64 st). Whole semitones go to Transpose.
      const double semis = 128.0 * (v - 0.5);
      const double whole = std::round(semis);
      masterTuneSemis = whole;
      set(G_MasterTune, std::clamp((semis - whole) * 100.0, -100.0, 100.0));
    }
    if (num(pp, "kParamVoiceAmp", v)) set(G_Amp, std::sqrt(std::max(0.0, v) / 2.0));
    if (num(pp, "kParamPortamentoTime", v)) set(G_PortaTime, portaTimeInv(v));
    if (num(pp, "kParamPortamentoCurve", v)) set(G_PortaCurve, v / 100.0);
    if (num(pp, "kParamPortaAlways", v)) set(G_PortaAlways, v);
    if (num(pp, "kParamPortaScaled", v)) set(G_PortaScaled, v);
    if (num(pp, "kParamBendRangeUp", v)) set(G_BendUp, v);
    if (num(pp, "kParamBendRangeDn", v)) set(G_BendDown, v);
    if (num(pp, "kParamModWheel", v)) set(G_ModWheel, v / 100.0);
    if (num(pp, "kParamMonoToggle", v)) set(G_Mono, v);
    if (num(pp, "kParamLegato", v)) set(G_Legato, v);
    if (num(pp, "kParamPolyCount", v)) set(G_Poly, v);
    if (num(pp, "kParamLimitSameNotePolyphony", v)) set(G_SameNote, v);
    if (num(pp, "kParamSwing", v)) set(G_Swing, v / 100.0);
    if (num(pp, "kParamSwingDiv", v)) set(G_SwingDiv, std::clamp(v - 1.0, 0.0, 3.0));
    if (num(pp, "kParamTranspose", v)) set(G_Transpose, v);
    if (masterTuneSemis != 0.0) {
      double t = 0;
      num(pp, "kParamTranspose", t);
      if (std::fabs(t + masterTuneSemis) > 48.0) note("Main tuning beyond 48 semitones: clamped");
      set(G_Transpose, std::clamp(t + masterTuneSemis, -48.0, 48.0));
    }
    if (num(pp, "kParamDirectVol", v)) set(G_DirectVol, std::sqrt(std::max(0.0, v) / 2.0));
    if (num(pp, "kParamFXBus1Vol", v)) set(G_Bus1Vol, std::sqrt(std::max(0.0, v) / 2.0));
    if (num(pp, "kParamFXBus2Vol", v)) set(G_Bus2Vol, std::sqrt(std::max(0.0, v) / 2.0));
    // J-203h, measured on the source (a bus with a clipper on the main rack): bus output 0 runs through the
    // main rack, 1 is the direct out (skips it), 2 feeds the other bus (no counterpart yet: main, its usual way)
    if (num(pp, "kParamFXBus1Dest", v)) set(G_Bus1Dest, v >= 0.5 && v < 1.5 ? 1 : 0);
    if (num(pp, "kParamFXBus2Dest", v)) set(G_Bus2Dest, v >= 0.5 && v < 1.5 ? 1 : 0);
    // output 2 = the other bus: Plasma's bus chain (one direction; when both buses name each
    // other, bus 1 feeds bus 2 and bus 2 goes to the main output)
    double d1 = 0, d2 = 0;
    num(pp, "kParamFXBus1Dest", d1), num(pp, "kParamFXBus2Dest", d2);
    set(tailP(T_BusChain), d1 >= 1.5 ? 1.0 : d2 >= 1.5 ? 2.0 : 0.0);
    if (num(pp, "kParamOversampling", v)) set(G_Quality, v);
    if (num(pp, "kParamS1Compatibility", v)) set(G_S1Mode, v);
    if (num(pp, "kParamGlobalTuning", v)) set(G_ConcertA, v);
    if (num(pp, "kParamNoteLatch", v)) set(G_NoteLatch, v);
    if (text(pp, "kParamVoicePriority", s)) {
      static const char* const k[] = {"Last", "First", "High", "Low", "Velocity"};
      set(G_Priority, std::max(0, indexOf(k, 5, s)));
    }
    if (text(pp, "kParamMidiOut", s)) {
      static const char* const k[] = {"Off", "ClipPlayer", "Synth"};
      set(G_MidiOut, std::max(0, indexOf(k, 3, s)));
    }
    if (const Json* mpe = o.find("mpeEnabled"); mpe && mpe->type == Json::Type::Bool) set(G_MpeOn, mpe->boolean ? 1 : 0);
    if (const Json* b = o.find("mpePitchBendRange"); b && b->type == Json::Type::Number) set(G_MpeBend, b->number);
    const Json* pq = o.find("PitchQuantizer0");
    const Json* qp = plain(pq);
    if (num(qp, "kParamKey", v)) set(G_Key, v);
    if (pq) {
      const std::string name = pq->text("scaleName");
      const auto& names = scaleNames();
      bool found = false;
      for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) set(G_Scale, static_cast<double>(i)), found = true;
      if (!found && num(qp, "kParamScale", v)) {
        set(G_Scale, v);
        note("Scale chosen by position in the list");
      }
    }
    // voice control
    const Json* vp = plain(o.find("VoicePanel0"));
    if (num(vp, "kParamGlobalRandomOscPan", v)) set(G_RndPan, v / 100.0);
    if (num(vp, "kParamGlobalRandomOscDetune", v)) {
      double x10 = 0;
      num(vp, "kParamGlobalRandomOscDetune10x", x10);
      set(G_RndDetune, v * (x10 >= 0.5 ? 10.0 : 1.0) / 5.0);
    }
    if (num(vp, "kParamGlobalRandomFilterCutoff", v)) set(G_RndCutoff, v / 100.0);
    if (num(vp, "kParamGlobalRandomEnvTime", v)) set(G_RndEnv, v / 100.0);
    if (num(vp, "kParamGlobalScalingEnvTime", v)) {
      set(G_EnvScale, std::pow(2.0, (v - 50.0) / 25.0));
      note("Envelope time scaling: approximated");
    }
    if (num(vp, "kParamGlobalScalingLfoTime", v)) {
      set(G_LfoScale, std::pow(2.0, (v - 50.0) / 25.0));
      note("LFO time scaling: approximated");
    }
    const char* chips[] = {"kParamOscS", "kParamOscA", "kParamOscB", "kParamOscC", "kParamOscN"};
    for (int i = 0; i < 5; ++i)
      if (num(vp, chips[i], v)) set(G_VcS + i, v);
    if (num(vp, "kParamVoiceCount", v)) set(G_VcSteps, v);
    for (int i = 0; i < 8; ++i) {
      VoiceStep& vs = m.voiceSeq[static_cast<size_t>(i)];
      const std::string p = "kParamVoice" + std::to_string(i + 1);
      if (num(vp, (p + "Pan").c_str(), v)) vs.pan = static_cast<float>(v / 100.0);
      if (num(vp, (p + "Detune").c_str(), v)) vs.detune = static_cast<float>(v / 100.0);
      if (num(vp, (p + "FilterCutoff").c_str(), v)) vs.cutoff = static_cast<float>(v / 100.0);
      if (num(vp, (p + "EnvTime").c_str(), v)) vs.env = static_cast<float>(v / 100.0);
      if (num(vp, (p + "Mod1").c_str(), v)) vs.mod1 = static_cast<float>(v / 100.0);
      if (num(vp, (p + "Mod2").c_str(), v)) vs.mod2 = static_cast<float>(v / 100.0);
    }
  }

  // ---------------------------------------------------------------- effects
  int nextSlot = 0;
  void effects(const Json& o) {
    for (int rack = 0; rack < 3; ++rack) {
      const Json* r = o.find("FXRack" + std::to_string(rack));
      const Json* list = r ? r->find("FX") : nullptr;
      if (!list || list->type != Json::Type::Array) continue;
      int parent = -1, band = 0, bandLeft[3] = {0, 0, 0}, bands = 0;
      for (size_t idx = 0; idx < list->array.size(); ++idx) {
        const Json& fx = list->array[idx];
        const int id = static_cast<int>(fx.num("type", -1));
        const int type = id >= 0 && id <= 15 ? id + 1 : FxNone;
        const Json* body = nullptr;
        bool serum2Body = true;  // Serum 1 states arrive as "FXSerum1" bodies with every value written
        for (const auto& kv : fx.object)
          if (kv.first.rfind("FX", 0) == 0 && kv.second.type == Json::Type::Object) body = &kv.second, serum2Body = kv.first != "FXSerum1";
        if (type == FxNone || !body) {
          note("An unknown effect module was left out");
          ++rep.skipped;
          continue;
        }
        if (nextSlot >= kFxSlots) {
          note("More than 24 effect modules: the rest were left out");
          ++rep.skipped;
          continue;
        }
        const int slot = nextSlot++;
        fxSlotOf[rack * 100 + static_cast<int>(idx)] = slot;
        // place: inside the current splitter band, or at rack level
        FxNode node{slot, rack, -1, 0};
        while (parent >= 0 && band < bands && bandLeft[band] == 0) ++band;
        if (parent >= 0 && band < bands) {
          node.parent = parent;
          node.band = band;
          --bandLeft[band];
        } else {
          parent = -1;
        }
        m.fx.push_back(node);
        set(fxP(slot, X_Type), type);
        for (int q = 0; q < static_cast<int>(fxParams(type).size()); ++q) {
          double v = 0;  // J-203c: a Serum 2 module's unset values are Serum's own defaults
          set(fxP(slot, X_P0 + q), serum2Body && fxSerumDefault(type, q, v) ? static_cast<float>(fxNorm(type, q, v)) : fxDefaultNorm(type, q));
        }
        fxValues(slot, type, *body);
        if (type >= FxSplitLH) {
          if (node.parent >= 0) note("Nested splitters are flattened");
          const Json* pp = plain(body);
          bands = type == FxSplitLMH ? 3 : 2;
          for (int b = 0; b < 3; ++b) {
            double v = 0;
            num(pp, ("kParamModuleCount" + std::to_string(b + 1)).c_str(), v);
            bandLeft[b] = b < bands ? std::max(0, static_cast<int>(v)) : 0;
          }
          parent = slot;
          band = 0;
        }
      }
    }
  }
  // J-201: the source's delay times, measured at 120 BPM. Free: TIME L / R are seconds
  // (0.075 when not stored). Synced: the stored value picks a straight division, 1/32
  // note to 4 bars, at these thresholds (1 beat when not stored). LINK uses TIME R for
  // both sides. Plasma's knobs take the matching control value.
  void delayTimes(int slot, const Json* pp) {
    auto raw = [&](const char* k, double d) {
      const Json* j = pp ? pp->find(k) : nullptr;
      return j && j->type == Json::Type::Number ? j->number : d;
    };
    const bool sync = raw("kParamBeatSync", 1) >= 0.5;
    const double def = sync ? 0.08 : 0.075;
    double tl = raw("kParamTimeL", def);
    const double tr = raw("kParamTimeR", def);
    if (raw("kParamLink", 0) >= 0.5) tl = tr;
    auto control = [&](double x) {
      if (!sync) return std::sqrt(std::max(0.0, (x - 0.001) / 1.999));
      static const double kEdge[] = {0.006, 0.016, 0.031, 0.0615, 0.105, 0.175, 0.275, 0.415};
      int k = 0;
      while (k < 8 && x >= kEdge[k]) ++k;
      const double beats = 0.0625 * std::exp2(k);  // 1/16 beat .. 16 beats
      if (beats > 4.0) note("Delay: synced times longer than one bar play one bar long");
      // Plasma's synced steps: 1/16, 1/8, 1/6, 3/16, 1/4, 1/3, 3/8, 1/2, 2/3, 3/4, 1, 4/3, 3/2, 2, 3, 4 beats
      static const int kIdx[] = {0, 1, 4, 7, 10, 13, 15, 15, 15};
      return kIdx[k] / 15.0;
    };
    const int qL = fxParamIndex(FxDelay, "kParamTimeL"), qR = fxParamIndex(FxDelay, "kParamTimeR");
    if (qL >= 0) set(fxP(slot, X_P0 + qL), control(tl));
    if (qR >= 0) set(fxP(slot, X_P0 + qR), control(tr));
  }
  void fxValues(int slot, int type, const Json& body) {
    const Json* pp = plain(&body);
    if (pp) {
      for (const auto& kv : pp->object) {
        const std::string& key = kv.first;
        if (key == "kParamEnable") {
          if (kv.second.type == Json::Type::Number) set(fxP(slot, X_On), kv.second.number >= 0.5 ? 1 : 0), used.insert(&kv.second);
          continue;
        }
        if (key.rfind("kParamModuleCount", 0) == 0) {
          used.insert(&kv.second);
          continue;
        }
        const int q = fxParamIndex(type, key.c_str());
        if (q < 0) {
          note(fxTypeNames()[static_cast<size_t>(type)] + ": " + key + " is not modelled");
          ++rep.skipped;
          continue;
        }
        const FxParamDef& d = fxParams(type)[static_cast<size_t>(q)];
        double plainV = 0;
        if (kv.second.type == Json::Type::Number) {
          plainV = kv.second.number;
        } else if (kv.second.type == Json::Type::String) {
          const std::string& s = kv.second.str;
          int k = -1;
          if (type == FxFilter && key == "kParamType") k = indexOf(kFilterKeys, kFilterKeyCount, s);
          else if (type == FxDistortion && key == "kParamMode") {
            static const char* const dm[] = {"kTube", "kSoftClip", "kHardClip", "kDiode1", "kDiode2", "kLinFold",
                                             "kSinFold", "kZeroSquare", "kDownsample", "kAsym", "kRectify", "kXShaper",
                                             "kXShaperAsym", "kSineShaper", "kStompBox", "kTapeSat", "kOverdrive", "kSoftSat"};
            k = indexOf(dm, 18, s);
          } else if (type == FxReverb && key == "kParamType") {
            static const char* const rt[] = {"kPlate", "kHall", "kVintage", "kAbyss", "kSpace"};
            k = indexOf(rt, 5, s);
          }
          if (k < 0) {
            note(fxTypeNames()[static_cast<size_t>(type)] + ": value " + s + " is not available");
            continue;
          }
          plainV = k;
        } else {
          continue;
        }
        (void)d;
        used.insert(&kv.second);
        set(fxP(slot, X_P0 + q), fxNorm(type, q, plainV));
      }
    }
    if (type == FxDelay) delayTimes(slot, pp);
    FxExtra& ex = m.fxExtra[static_cast<size_t>(slot)];
    if (type == FxDistortion) {
      // X-Shaper curves live next to the module ("flex": A, B)
      (void)ex;
    }
    if (type == FxConvolve) {
      const std::string rel = body.text("relativePathToIR");
      std::string path;
      if (const Json* e = body.find("embeddedIR"); e && files) {
        std::vector<std::vector<float>> ch;
        if (serum::channelsOf(*e, ch) && !ch[0].empty())
          path = addMedia(m, rel.empty() ? std::string("Impulse") : fs::path(rel).filename().string(),
                          static_cast<int>(std::lround(body.num("sampleRate", 44100))), ch);
      }
      if (path.empty()) path = resolve(rel, {"Impulses", "Impulses/Factory", "Impulses/User", ""});
      // J-203d: with no impulse at all the source passes the input unchanged (measured: mix 100 gives the
      // input to 0.01 dB, in phase), so an empty slot gets a one-sample impulse
      if (path.empty() && rel.empty() && !body.find("embeddedIR") && files) path = addMedia(m, "Impulse", 48000, {{1.f}});
      const int qi = fxParamIndex(FxConvolve, "kParamIr");
      if (!path.empty()) {
        ex.ir = path;
        set(fxP(slot, X_P0 + qi), fxNorm(FxConvolve, qi, 0));
      } else {
        if (!rel.empty()) missing(rel);
        note("Impulse response not found: a built-in room plays instead");
        set(fxP(slot, X_P0 + qi), fxNorm(FxConvolve, qi, 2));
      }
    }
    if (type == FxHyper || type == FxPhaser || type == FxChorus) {
      // free-running modulation phases stored with the preset start fresh
    }
  }
  void xshaper(const Json& o) {
    // "flex" curves sit on the FX entry (beside the module body)
    for (int rack = 0; rack < 3; ++rack) {
      const Json* r = o.find("FXRack" + std::to_string(rack));
      const Json* list = r ? r->find("FX") : nullptr;
      if (!list || list->type != Json::Type::Array) continue;
      for (size_t idx = 0; idx < list->array.size(); ++idx) {
        auto it = fxSlotOf.find(rack * 100 + static_cast<int>(idx));
        if (it == fxSlotOf.end()) continue;
        const Json* fl = list->array[idx].find("flex");
        if (!fl || fl->type != Json::Type::Array) continue;
        FxExtra& ex = m.fxExtra[static_cast<size_t>(it->second)];
        // the X-Shaper's curves bend the other way from the source's other curves (J-203k, fitted against the
        // hosted plugin's output with bent curves): flip the tension
        auto bent = [](Curve cv) {
          for (CurvePt& q : cv) q.c = -q.c;
          return cv;
        };
        if (fl->array.size() > 0) ex.shapeA = bent(curve(&fl->array[0]));
        // an empty second curve is the straight line (the drive then fades the first curve back to clean)
        if (fl->array.size() > 1) ex.shapeB = bent(curve(&fl->array[1]));
        if (!ex.shapeA.empty() && ex.shapeB.empty()) ex.shapeB = Curve{{0, 0, 0}, {1, 1, 0}};
      }
    }
  }

  // ---------------------------------------------------------------- matrix
  int destParam(const std::string& type, int id, const std::string& key) {
    auto inMap = [&](const KeyMap* maps, size_t n, int base) {
      for (size_t i = 0; i < n; ++i)
        if (key == maps[i].key) return base + maps[i].param;
      return -1;
    };
    if (type == "Oscillator") {
      if (id >= 0 && id < 3) return inMap(kOscMain, sizeof(kOscMain) / sizeof(kOscMain[0]), oscP(id, 0));
      if (id == 3) {
        if (key == "kParamVolume") return noiseP(N_Level);
        if (key == "kParamPan") return noiseP(N_Pan);
        if (key == "kParamCoarsePit" || key == "kParamPitch") return noiseP(N_Pitch);
      }
      if (id == 4) {
        if (key == "kParamVolume") return subP(S_Level);
        if (key == "kParamPan") return subP(S_Pan);
        if (key == "kParamCoarsePit") return subP(S_Coarse);
        if (key == "kParamOctave") return subP(S_Octave);
      }
      return -1;
    }
    if (type == "WTOsc" || type == "MultiSampleOsc" || type == "SampleOsc" || type == "GranularOsc" || type == "SpectralOsc") {
      if (id < 0 || id >= 3) return -1;
      if (key == "kParamTablePos") return oscP(id, O_WtPos);
      if (key == "kParamDensity") return oscP(id, O_GDensity);
      if (key == "kParamGrainLength") return oscP(id, O_GLength);
      if (key == "kParamRandomPitch") return oscP(id, O_GRndPitch);
      if (key == "kParamFreqLo") return oscP(id, O_SLo);
      if (key == "kParamFreqHi") return oscP(id, O_SHi);
      return inMap(kOscSub, sizeof(kOscSub) / sizeof(kOscSub[0]), oscP(id, 0));
    }
    if (type == "NoiseOsc") {
      if (key == "kParamColor") return noiseP(N_Pitch);  // the source's noise pitch knob
      if (key == "kParamFine") return noiseP(N_Fine);
      if (key == "kParamInitialPhase") return noiseP(N_Phase);
      return -1;
    }
    if (type == "SubOsc") return key == "kParamInitialPhase" ? subP(S_Phase) : -1;
    if (type == "VoiceFilter" && id >= 0 && id < kFilters) return inMap(kFiltMap, sizeof(kFiltMap) / sizeof(kFiltMap[0]), filtP(id, 0));
    if (type == "Env" && id >= 0 && id < kEnvs) return inMap(kEnvMap, sizeof(kEnvMap) / sizeof(kEnvMap[0]), envP(id, 0));
    if (type == "LFO" && id >= 0 && id < kLfos) {
      if (key == "kParamRise") return lfoP(id, L_Rise);
      if (key == "kParamDelay") return lfoP(id, L_Delay);
      return inMap(kLfoMap, sizeof(kLfoMap) / sizeof(kLfoMap[0]), lfoP(id, 0));
    }
    if (type == "Macro" && id >= 0 && id < kMacros) return macroP(id);
    if (type == "RetriggerState") {
      static const char* const k[kRetrigs] = {"kParamNote", "kParamOscA", "kParamOscB", "kParamOscC", "kParamSub", "kParamNoise", "kParamEnv2",
                                              "kParamEnv3", "kParamEnv4", "kParamLFO1", "kParamLFO2", "kParamLFO3", "kParamLFO4", "kParamLFO5",
                                              "kParamLFO6", "kParamLFO7", "kParamLFO8", "kParamLFO9", "kParamLFO10"};
      const int i = indexOf(k, kRetrigs, key);
      return i >= 0 ? retrigP(i) : -1;
    }
    if (type == "LFOPointModBus" && id >= 0 && id < kLfoBusses) return busP(id);
    if (type == "RoutingSlot" && id >= 0 && id < kRoutes) return inMap(kRouteMap, sizeof(kRouteMap) / sizeof(kRouteMap[0]), routeP(id, 0));
    if (type == "Global") {
      static const std::pair<const char*, int> g[] = {{"kParamMasterVolume", G_MasterVol}, {"kParamMasterTuning", G_MasterTune},
                                                     {"kParamVoiceAmp", G_Amp},           {"kParamPortamentoTime", G_PortaTime},
                                                     {"kParamPortamentoCurve", G_PortaCurve}, {"kParamTranspose", G_Transpose},
                                                     {"kParamDirectVol", G_DirectVol},   {"kParamFXBus1Vol", G_Bus1Vol},
                                                     {"kParamFXBus2Vol", G_Bus2Vol},     {"kParamSwing", G_Swing},
                                                     {"kParamModWheel", G_ModWheel}};
      for (const auto& [k, p] : g)
        if (key == k) return p;
      return -1;
    }
    if (type == "VoicePanel") {
      if (key == "kParamGlobalScalingEnvTime") return G_EnvScale;
      if (key == "kParamGlobalScalingLfoTime") return G_LfoScale;
      if (key == "kParamGlobalRandomOscPan") return G_RndPan;
      if (key == "kParamGlobalRandomFilterCutoff") return G_RndCutoff;
      return -1;
    }
    if (type == "Arp") {
      static const char* const k[] = {"kParamRate", "kParamTransposeShift", "kParamTransposeRange", "kParamOffset", "kParamRepeats",
                                      "kParamGate", "kParamChance", "kParamRetrigRate", "kParamVeloDecay", "kParamVeloTarget",
                                      "kParamTranspose", "kParamWrapTranspose", "kParamWrapRange", "kParamWrapPhantomNote"};
      const int i = indexOf(k, 14, key);
      return i >= 0 ? G_ArpRate + i : -1;
    }
    if (type == "MidiClip" || type == "ClipPlayer") {
      if (key == "kParamTranspose") return G_ClipTranspose;
      if (key == "kParamRate") return G_ClipRate;
      if (key == "kParamOffset") return G_ClipOffset;
      return -1;
    }
    if (type.rfind("FX", 0) == 0) {
      auto it = fxSlotOf.find(id);
      if (it == fxSlotOf.end()) return -1;
      const int slot = it->second;
      const int ft = static_cast<int>(std::lround(st.params[static_cast<size_t>(fxP(slot, X_Type))]));
      const int q = fxParamIndex(ft, key.c_str());
      return q >= 0 ? fxP(slot, X_P0 + q) : -1;
    }
    return -1;
  }
  void matrix(const Json& o) {
    int out = 0;
    for (int s = 0; s < 64; ++s) {
      const Json* slot = o.find("ModSlot" + std::to_string(s));
      if (!slot) continue;
      const Json* src = slot->find("source");
      int mainId = 0, auxId = 0;
      if (src && src->type == Json::Type::Array) {
        if (src->array.size() > 0) mainId = static_cast<int>(src->array[0].number);
        if (src->array.size() > 1) auxId = static_cast<int>(src->array[1].number);
      }
      if (mainId == 0) {  // an empty slot: its settings do nothing
        if (const Json* pp = plain(slot))
          for (const auto& kv : pp->object) used.insert(&kv.second);
        continue;
      }
      const int ms = sourceOf(mainId), as = sourceOf(auxId);
      const std::string type = slot->text("destModuleTypeString");
      const int id = static_cast<int>(slot->num("destModuleID", 0));
      const std::string key = slot->text("destModuleParamName");
      int dest = destParam(type, id, key);
      const bool noiseKnob = dest == noiseP(N_Pitch) && type == "NoiseOsc";
      if (noiseKnob && builtinNoise) dest = tailP(T_NoiseTone);  // the knob is a tone filter there
      if (noiseKnob && geigerNoise) dest = -1;
      if (ms < 0 || dest < 0 || !modulatable(dest)) {
        note("Matrix: " + (dest < 0 ? type + " " + key + " is not a Plasma destination" : "a source is not available"));
        ++rep.skipped;
        continue;
      }
      if (out >= kModSlots) break;
      const Json* pp = plain(slot);
      double v;
      set(modP(out, M_Source), ms);
      set(modP(out, M_Dest), dest + 1);
      double amt = num(pp, "kParamAmount", v) ? v / 100.0 : 0.0;
      if (noiseKnob && !builtinNoise) amt *= noisePitchModScale;
      set(modP(out, M_Amount), amt);
      if (num(pp, "kParamBipolar", v)) set(modP(out, M_Bipolar), v);
      if (num(pp, "kParamOut", v)) set(modP(out, M_Out), v / 100.0);
      if (num(pp, "kParamBypass", v)) set(modP(out, M_Bypass), v);
      if (num(pp, "kParamCurveIn", v)) set(modP(out, M_Curve), v / 100.0);
      if (num(pp, "kParamAuxCurve", v)) set(modP(out, M_AuxCurve), v / 100.0);
      if (num(pp, "kParamAuxInverted", v)) set(modP(out, M_AuxInv), v);
      // J-203g: the source's route smoothing (rise / fall, linked or not) left velocity, envelope
      // and LFO routes unchanged at every setting measured (10..100 %), so it is not carried over
      if (num(pp, "kParamCurveOut", v) && std::fabs(v) > 1e-3) note("Matrix output curve: approximated by the input curve");
      if (num(pp, "kParamDelayOffset", v)) note("Matrix delay offset is not modelled");
      if (as > SrcNone) set(modP(out, M_Aux), as);
      if (const Json* fl = slot->find("flex"); fl && fl->type == Json::Type::Array) {
        double mainCd = 0, auxCd = 0;
        num(pp, "kParamMainCurveData", mainCd);
        num(pp, "kParamAuxCurveData", auxCd);
        size_t k = 0;
        if (mainCd >= 0.5 && k < fl->array.size()) m.modCurve[static_cast<size_t>(out)] = curve(&fl->array[k++]);
        if (auxCd >= 0.5 && k < fl->array.size()) m.auxCurve[static_cast<size_t>(out)] = curve(&fl->array[k]);
      }
      ++out;
    }
  }

  // ---------------------------------------------------------------- arp / clips
  static std::vector<ClipNote> clipNotes(const Json* clip, float& endBeats) {
    std::vector<ClipNote> out;
    if (!clip) return out;
    endBeats = static_cast<float>(clip->num("regionEndBeats", 0));
    const Json* ns = clip->find("notes");
    if (!ns || ns->type != Json::Type::Array) return out;
    for (const auto& n : ns->array) {
      ClipNote c;
      c.start = static_cast<float>(n.num("timeStamp", 0));
      c.len = static_cast<float>(std::max(0.01, n.num("length", 0.25)));
      c.key = std::clamp(static_cast<int>(n.num("noteNum", 60)), 0, 127);
      const Json* at = n.find("attributes");
      c.vel = at && at->type == Json::Type::Array && !at->array.empty() ? static_cast<float>(std::clamp(at->array[0].number, 0.0, 1.0)) : 0.8f;
      const Json* mu = n.find("muted");
      c.mute = mu && ((mu->type == Json::Type::Bool && mu->boolean) || (mu->type == Json::Type::Number && mu->number != 0));
      out.push_back(c);
      if (out.size() >= 4096) break;
    }
    return out;
  }
  void sequencers(const Json& o) {
    const Json* ap = plain(o.find("Arp0"));
    double v;
    if (num(ap, "kParamEnabled", v)) set(G_ArpOn, v);
    if (num(ap, "kParamActiveClipID", v)) m.arpActive = std::clamp(static_cast<int>(v), 0, kArpSlots - 1);
    if (num(ap, "kParamLaunchQuantize", v)) m.arpLaunchQuant = std::clamp(static_cast<int>(v), 0, 15);
    for (int s = 0; s < kArpSlots; ++s) {
      const Json* ac = o.find("ArpClip" + std::to_string(s));
      if (!ac) continue;
      ArpSlot& a = m.arp[static_cast<size_t>(s)];
      a.name = ac->text("name", a.name);
      const Json* pp = plain(ac);
      std::string str;
      if (text(pp, "kParamShape", str)) {
        static const char* const k[] = {"Pattern", "Up", "Down", "UpDown", "DownUp", "UpAndDown", "DownAndUp", "ThumbUp", "ThumbUD", "PinkyUp",
                                        "PinkyUD", "Converge", "Diverge", "ConvAndDiv", "Played", "Chord", "Rand", "RandNoDup", "RandDrift", "RandOnce"};
        a.shape = std::max(0, indexOf(k, 20, str));
      }
      // J-201: the rate knob steps through straight divisions, one per ninth of its range around
      // 1/16 at 0.5 (measured at 120 BPM: 0.34..0.44 -> 1/8, 0.46..0.54 -> 1/16, 0.56..0.66 -> 1/32,
      // 0.68.. -> 1/64 and faster); "dotted" plays the next faster division dotted (1/16 dotted =
      // 0.1875 beats). Plasma's straight divisions run 8 bars (0) .. 1/64 (9), 1/16 = 7.
      double rv = 0.5;
      num(pp, "kParamRate", rv);
      double dotted = 0;
      num(pp, "kParamDotted", dotted);
      if (a.shape == ArpPattern) dotted = 0;  // measured: a pattern keeps straight steps when dotted is on
      const int k = static_cast<int>(std::lround(9.0 * (rv - 0.5))) + (dotted >= 0.5 ? 1 : 0);
      if (7 + k > 9 || 7 + k < 0) note("Arp rate beyond Plasma's divisions: clamped");
      a.rate = std::clamp(7 + k, 0, 9);
      a.dot = dotted >= 0.5;
      if (num(pp, "kParamBeatSync", v)) a.bpm = v >= 0.5;
      if (num(pp, "kParamTriplets", v)) a.trip = v >= 0.5;
      if (num(pp, "kParamTransposeShift", v)) a.shift = static_cast<int>(v);
      if (num(pp, "kParamTransposeRange", v)) a.range = static_cast<int>(v);
      if (num(pp, "kParamThru", v)) a.thru = v >= 0.5;
      if (num(pp, "kParamOffset", v)) a.offset = static_cast<int>(v);
      if (num(pp, "kParamRepeats", v)) a.repeats = static_cast<int>(v);
      if (num(pp, "kParamGate", v)) a.gate = static_cast<float>(v / 100.0);
      if (num(pp, "kParamChance", v)) a.chance = static_cast<float>(v / 100.0);
      if (num(pp, "kParamLaunchRetrig", v)) a.retrigLaunch = v >= 0.5;
      if (num(pp, "kParamBeatRetrig", v)) a.retrigRate = v >= 0.5;
      if (num(pp, "kParamNoteRetrig", v)) a.retrigNote = v >= 0.5;
      if (num(pp, "kParamFirstNoteRetrig", v)) a.retrigFirst = v >= 0.5;
      if (num(pp, "kParamRetrigRate", v)) a.retrigDiv = static_cast<int>(v);
      if (num(pp, "kParamVeloEnabled", v)) a.velo = v >= 0.5;
      if (num(pp, "kParamVeloRetrig", v)) a.veloRetrig = v >= 0.5;
      if (num(pp, "kParamVeloDecay", v)) a.veloDecay = static_cast<float>(v);
      if (num(pp, "kParamVeloTarget", v)) a.veloTarget = static_cast<float>(v / 100.0);
      if (num(pp, "kParamWrapRange", v)) a.rangeWrap = static_cast<int>(v);
      // J-201: pattern rows past the held notes repeat them at the same pitch by default
      // (measured: rows 1, 2 and 12 over one held key all play that key)
      a.wrap = 2;
      if (text(pp, "kParamRangeWrapMode", str)) note("Arp pattern wrap mode " + str + ": rows past the held notes repeat them");
      float end = 0;
      auto ns = clipNotes(ac->find("clip"), end);
      for (const auto& n : ns) a.steps.push_back({n.start, n.len, std::clamp(n.key, 0, 31), n.vel, false, 0});
      if (end > 0) a.patternBeats = end;
    }
    const Json* cp = plain(o.find("ClipPlayer0"));
    bool clipOn = false;
    if (num(cp, "kParamEnabled", v)) set(G_ClipOn, v), clipOn = v >= 0.5;
    if (num(cp, "kParamMonoClipTrigger", v)) m.clipPoly = v < 0.5;
    if (const Json* c = o.find("ClipPlayer0"); c) {
      if (const Json* act = c->find("activeClips"); act && act->type == Json::Type::Array && !act->array.empty()) {
        m.clipActive = std::clamp(static_cast<int>(act->array[0].number), 0, kClipSlots - 1);
      } else if (clipOn) {
        // J-201: with no active clip the source passes the keys through (measured:
        // such presets play like the clip player is off), so nothing plays from clips.
        // J-203i: unless the player names a keyboard-span clip in mono span mode that loops: then
        // the keys play that clip, transposed (measured over the library: with that rule 2 more
        // presets pass and 3 come within a few dB; the poly, one-shot and static span clips match
        // far better played through, so they stay off)
        double sc = -1;
        std::string mode, pmode;
        const Json* scp = num(cp, "kParamSpanKeyboardClip", sc) && sc >= 0 && sc < kClipSlots
                              ? plain(o.find("MidiClip" + std::to_string(static_cast<int>(sc))))
                              : nullptr;
        text(scp, "kParamPlaybackMode", pmode);
        const bool span = scp && text(scp, "kParamSpanKeyboardMode", mode) && mode == "Mono" && pmode != "Static" && pmode != "OneShot";
        if (span) {
          m.clipActive = static_cast<int>(sc);
        } else {
          set(G_ClipOn, 0);
          clipOn = false;
        }
      }
    }
    for (int s = 0; s < kClipSlots; ++s) {
      const Json* mc = o.find("MidiClip" + std::to_string(s));
      if (!mc) continue;
      ClipSlot& c = m.clip[static_cast<size_t>(s)];
      c.name = mc->text("name", c.name);
      float end = 0;
      c.notes = clipNotes(mc->find("clip"), end);
      if (end > 0) c.lengthBeats = end;
      const Json* pp = plain(mc);
      std::string str;
      if (text(pp, "kParamPlaybackMode", str)) {
        static const char* const k[] = {"Normal", "Reverse", "Pendulum", "Random", "RandomNoDup", "RandomStart", "RandomEnd", "OneShot", "Static"};
        c.mode = std::max(0, indexOf(k, 9, str));
      }
      if (text(pp, "kParamSpanKeyboardMode", str)) {
        static const char* const k[] = {"Off", "Mono", "Poly", "Offset"};
        c.span = std::max(0, indexOf(k, 4, str));
      }
      if (num(pp, "kParamTranspose", v)) c.transpose = static_cast<int>(v);
      if (num(pp, "kParamPlaybackModeTime", v)) c.modeTime = static_cast<float>(v);
      if (num(pp, "kParamTriplets", v)) c.trip = v >= 0.5;
      if (num(pp, "kParamDotted", v)) c.dot = v >= 0.5;
      if (num(pp, "kParamBeatSync", v)) c.bpm = v >= 0.5;
      if (num(pp, "kParamLaunchQuantize", v)) c.launchQuant = static_cast<int>(v);
      if (num(pp, "kParamLaunchRetrig", v)) c.retrig = v >= 0.5;
      if (num(pp, "kParamVelocityTrigger", v)) c.veloTrig = v >= 0.5;
      if (num(pp, "kParamNoteGate", v)) c.noteGate = v >= 0.5;
      if (num(pp, "kParamRate", v)) note("Clip rate: plays at the normal speed");
      // automation lanes to macros
      if (const Json* cl = mc->find("clip"))
        if (const Json* au = cl->find("automation"); au && au->type == Json::Type::Array)
          for (const auto& lane : au->array) {
            const std::string dest = lane.text("dest");
            ClipLane L;
            const Json* ts = lane.find("timeStamps");
            const Json* evs = lane.find("events");
            const bool none = (!ts || ts->type != Json::Type::Array || ts->array.empty()) && (!evs || evs->type != Json::Type::Array || evs->array.empty());
            if (none) continue;
            if (dest.rfind("kAutoDest_Macro", 0) == 0) L.target = macroP(std::clamp(std::atoi(dest.c_str() + 15), 0, kMacros - 1));
            else if (dest == "kAutoDest_CC1") L.target = G_ModWheel;
            else if (dest == "kAutoDest_PitchBend") L.target = G_PitchWheel;
            else {
              if (clipOn) note("Clip automation of MIDI controllers other than the mod wheel and pitch bend is not played");
              continue;
            }
            const Json* vs = lane.find("values");
            if (ts && vs && ts->type == Json::Type::Array && vs->type == Json::Type::Array)
              for (size_t i = 0; i < std::min(ts->array.size(), vs->array.size()); ++i)
                L.pts.push_back({static_cast<float>(ts->array[i].number), static_cast<float>(vs->array[i].number)});
            if (const Json* ev = lane.find("events"); ev && ev->type == Json::Type::Array)
              for (const auto& e : ev->array) L.pts.push_back({static_cast<float>(e.num("timeStamp")), static_cast<float>(e.num("value"))});
            if (!L.pts.empty()) c.lanes.push_back(L);
          }
    }
  }
};

std::vector<uint8_t> readFile(const std::string& path, std::string& err) {
  std::vector<uint8_t> d;
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) {
    err = "cannot open the file";
    return d;
  }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  if (n <= 0 || n > (std::streamoff(512) << 20)) {
    err = n <= 0 ? "the file is empty" : "the file is too large";
    return d;
  }
  f.seekg(0);
  d.resize(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(d.data()), n);
  if (!f) {
    err = "cannot read the file";
    d.clear();
  }
  return d;
}

}  // namespace

std::vector<std::string> serumRootCandidates() {
  std::vector<std::string> c;
  // dev / tests: GL_PLASMA_SERUM_ROOTS="<folder>;<folder>" replaces the system's folders
  if (const char* env = std::getenv("GL_PLASMA_SERUM_ROOTS"); env && *env) {
    const std::string s = env;
    size_t s0 = 0;
    while (s0 <= s.size()) {
      size_t s1 = s.find(';', s0);
      if (s1 == std::string::npos) s1 = s.size();
      if (s1 > s0) c.push_back(s.substr(s0, s1 - s0));
      s0 = s1 + 1;
    }
    return c;
  }
  const char* homeVar = platform::homeEnv();
  const std::string home = homeVar ? homeVar : "";
#if defined(_WIN32)
  if (const char* pd = std::getenv("PROGRAMDATA")) {
    c.push_back(std::string(pd) + "\\Xfer\\Serum 2 Presets");
    c.push_back(std::string(pd) + "\\Xfer\\Serum Presets");
  }
  if (const char* up = std::getenv("USERPROFILE")) {
    c.push_back(std::string(up) + "\\Documents\\Xfer\\Serum 2 Presets");
    c.push_back(std::string(up) + "\\Documents\\Xfer\\Serum Presets");
  }
#else
  c.push_back("/Library/Audio/Presets/Xfer Records/Serum 2 Presets");
  c.push_back("/Library/Audio/Presets/Xfer Records/Serum Presets");
  if (!home.empty()) {
    c.push_back(home + "/Library/Audio/Presets/Xfer Records/Serum 2 Presets");
    c.push_back(home + "/Library/Audio/Presets/Xfer Records/Serum Presets");
    c.push_back(home + "/Documents/Xfer/Serum 2 Presets");
    c.push_back(home + "/Documents/Xfer/Serum Presets");
    // Wine prefixes (Linux): the Windows installers' folders
    std::string wine = home + "/.wine";
    if (const char* wp = std::getenv("WINEPREFIX"); wp && *wp) wine = wp;
    c.push_back(wine + "/drive_c/ProgramData/Xfer/Serum 2 Presets");
    c.push_back(wine + "/drive_c/ProgramData/Xfer/Serum Presets");
    std::error_code ec;
    const fs::path users = fs::path(wine) / "drive_c" / "users";
    if (fs::is_directory(users, ec))
      for (const auto& u : fs::directory_iterator(users, fs::directory_options::skip_permission_denied, ec)) {
        c.push_back((u.path() / "Documents" / "Xfer" / "Serum 2 Presets").string());
        c.push_back((u.path() / "Documents" / "Xfer" / "Serum Presets").string());
      }
  }
#endif
  return c;
}

std::vector<std::string> defaultSerumRoots() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& p : serumRootCandidates())
    if (fs::is_directory(fs::u8path(p), ec)) out.push_back(p);
  for (const auto& p : extraSerumFolders())
    if (fs::is_directory(fs::u8path(p), ec) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  return out;
}

namespace {
std::mutex gExtraMutex;
std::vector<std::string> gExtraFolders;
}  // namespace

void setExtraSerumFolders(const std::vector<std::string>& folders) {
  std::lock_guard<std::mutex> lk(gExtraMutex);
  gExtraFolders = folders;
}
std::vector<std::string> extraSerumFolders() {
  std::lock_guard<std::mutex> lk(gExtraMutex);
  return gExtraFolders;
}

namespace serum {

int filterKeyCount() { return kFilterKeyCount; }
const char* filterKey(int i) { return i >= 0 && i < kFilterKeyCount ? kFilterKeys[i] : ""; }
const char* warpKey(int i) { return i >= 0 && i < 70 ? kWarpKeys[i] : ""; }

Json floatBlob(const float* p, size_t n) {
  std::string b(kBlobTag);
  b.append(reinterpret_cast<const char*>(p), n * sizeof(float));
  return Json::makeString(std::move(b));
}

bool floatsOf(const Json& j, std::vector<float>& out) {
  out.clear();
  const size_t tag = sizeof(kBlobTag) - 1;
  if (j.type == Json::Type::String) {
    if (j.str.size() < tag || j.str.compare(0, tag, kBlobTag) != 0) return false;
    const size_t n = (j.str.size() - tag) / sizeof(float);
    out.resize(n);
    if (n) std::memcpy(out.data(), j.str.data() + tag, n * sizeof(float));
  } else if (j.type == Json::Type::Array) {
    out.reserve(j.array.size());
    for (const auto& v : j.array) {
      if (v.type != Json::Type::Number) {
        out.clear();
        return false;
      }
      out.push_back(static_cast<float>(v.number));
    }
  } else {
    return false;
  }
  for (float& v : out)
    if (!std::isfinite(v)) v = 0.f;
  return !out.empty();
}

bool channelsOf(const Json& j, std::vector<std::vector<float>>& out) {
  out.clear();
  std::vector<float> one;
  if (floatsOf(j, one)) {
    out.push_back(std::move(one));
    return true;
  }
  if (j.type != Json::Type::Array) return false;
  for (const auto& c : j.array) {
    if (out.size() >= 2 || !floatsOf(c, one)) break;
    out.push_back(std::move(one));
  }
  if (out.size() == 2 && out[1].size() != out[0].size()) out[1].resize(out[0].size(), 0.f);
  return !out.empty();
}

bool decodeXferJson(const uint8_t* p, size_t n, Json& meta, Json& root, std::string& err) {
  if (n < 17 + 8 || std::memcmp(p, "XferJson", 8) != 0 || p[8] != 0) {
    err = "not a Serum 2 preset";
    return false;
  }
  uint64_t ml = 0;
  for (int i = 0; i < 8; ++i) ml |= static_cast<uint64_t>(p[9 + static_cast<size_t>(i)]) << (8 * i);
  if (ml > n - 17 || n - 17 - ml < 8) {
    err = "damaged preset header";
    return false;
  }
  const std::string metaText(reinterpret_cast<const char*>(p + 17), static_cast<size_t>(ml));
  if (!jsonParse(metaText, meta) || meta.type != Json::Type::Object) {
    err = "damaged preset header";
    return false;
  }
  const size_t body = 17 + static_cast<size_t>(ml);
  const uint32_t rawSize = p[body] | (p[body + 1] << 8) | (p[body + 2] << 16) | (static_cast<uint32_t>(p[body + 3]) << 24);
  std::vector<uint8_t> raw;
  if (!zstdDecompress(p + body + 8, n - body - 8, raw, &err)) return false;
  if (raw.size() != rawSize) {
    err = "damaged compressed data (size mismatch)";
    return false;
  }
  if (!cborDecode(raw.data(), raw.size(), root, &err) || root.type != Json::Type::Object) {
    if (err.empty()) err = "damaged preset data";
    return false;
  }
  return true;
}

void runImporter(const Json& meta, const Json& root, PluginState& out, ImportReport& rep, const std::vector<std::string>& roots, bool files,
                 const std::string& presetDir) {
  out = defaultPluginState(kType);
  Importer im{out, rep};
  if (!im.info) {
    rep.error = "Plasma is not available";
    rep.ok = false;
    return;
  }
  im.m = defaultModel();
  im.files = files;
  im.roots = files ? (roots.empty() ? defaultSerumRoots() : roots) : std::vector<std::string>{};
  if (files && !presetDir.empty()) {  // the preset's folder and up to three above it (pack layouts)
    fs::path d = fs::u8path(presetDir);
    for (int k = 0; k < 4 && !d.empty(); ++k, d = d.parent_path()) {
      im.near.push_back(d.u8string());
      if (d == d.root_path() || d.parent_path() == d) break;
    }
  }
  rep.presetName = meta.text("presetName", root.text("presetName"));
  rep.author = meta.text("presetAuthor", root.text("presetAuthor"));
  im.m.presetName = rep.presetName;
  im.m.author = rep.author;
  im.m.description = meta.text("presetDescription");
  if (const Json* tg = meta.find("tags"); tg && tg->type == Json::Type::Array)
    for (const auto& t : tg->array)
      if (t.type == Json::Type::String) im.m.tags.push_back(t.str);
  im.global(root);
  im.oscillators(root);
  im.noiseSub(root);
  im.filters(root);
  im.envLfoMacros(root);
  im.effects(root);
  im.xshaper(root);
  im.matrix(root);
  im.sequencers(root);
  if (files) im.tuning(root);
  im.unread(root, "");
  rep.approximated.insert(rep.approximated.end(), im.notes.begin(), im.notes.end());
  std::sort(rep.approximated.begin(), rep.approximated.end());
  rep.approximated.erase(std::unique(rep.approximated.begin(), rep.approximated.end()), rep.approximated.end());
  im.m.openFormat = rep.format.empty() ? "Serum preset" : rep.format;
  im.m.openNotes = rep.approximated;
  im.m.openMissing = rep.missingFiles;
  if (files) out.data = modelToData(im.m);
  rep.ok = true;
}

}  // namespace serum

bool importSerumBytes(const std::vector<uint8_t>& bytes, PluginState& out, ImportReport& rep, const std::vector<std::string>& searchRoots,
                      const std::string& presetDir) {
  rep = ImportReport{};
  const size_t n = bytes.size();
  Json meta, root;
  std::string err;
  if (n >= 4 && std::memcmp(bytes.data(), "CcnK", 4) == 0) {
    rep.format = "Serum 1 preset (.fxp)";
    serum::Serum1 s1;
    if (!serum::decodeSerum1(bytes.data(), n, s1, err)) {
      rep.error = err;
      return false;
    }
    std::vector<std::string> notes;
    serum::serum1Root(s1, meta, root, notes);
    rep.approximated = notes;
  } else {
    rep.format = "Serum 2 preset";
    if (!serum::decodeXferJson(bytes.data(), n, meta, root, err)) {
      rep.error = err;
      return false;
    }
  }
  serum::runImporter(meta, root, out, rep, searchRoots, true, presetDir);
  return rep.ok;
}

bool importSerumPreset(const std::string& path, PluginState& out, ImportReport& rep, const std::vector<std::string>& searchRoots) {
  std::string err;
  auto bytes = readFile(path, err);
  if (bytes.empty()) {
    rep = ImportReport{};
    rep.error = err;
    return false;
  }
  const bool ok = importSerumBytes(bytes, out, rep, searchRoots, fs::u8path(path).parent_path().u8string());
  if (ok && rep.presetName.empty()) rep.presetName = fs::u8path(path).stem().string();
  return ok;
}

}  // namespace gl::packs::plasma

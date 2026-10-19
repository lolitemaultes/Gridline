// Gridline — ZIP archives. See Zip.h.
#include "Zip.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace gl {

namespace {

bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return rd16(p) | (static_cast<uint32_t>(rd16(p + 2)) << 16); }

void le16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void le32(std::vector<uint8_t>& b, uint32_t v) {
  le16(b, v & 0xFFFF);
  le16(b, v >> 16);
}

// DOS date/time of 2026-01-01 00:00 (entries carry no meaningful time).
constexpr uint16_t kDosTime = 0, kDosDate = ((2026 - 1980) << 9) | (1 << 5) | 1;

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
  static uint32_t table[256];
  static bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)init;
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---------------------------------------------------------------- writer

ZipWriter::~ZipWriter() {
  if (f_) std::fclose(f_);
}

bool ZipWriter::open(const std::string& path, std::string* err) {
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return fail(err, "cannot write " + path);
  entries_.clear();
  pos_ = 0;
  ok_ = true;
  return true;
}

void ZipWriter::raw(const void* p, size_t n) {
  if (!f_ || std::fwrite(p, 1, n, f_) != n) ok_ = false;
  pos_ += static_cast<uint32_t>(n);
}

bool ZipWriter::add(const std::string& name, const std::vector<uint8_t>& data) {
  if (!f_ || data.size() > 0xFFFFFFF0u || static_cast<uint64_t>(pos_) + data.size() > 0xFFFFFFF0u) return ok_ = false;
  Entry e;
  e.name = name;
  e.crc = crc32(data.data(), data.size());
  e.size = static_cast<uint32_t>(data.size());
  e.offset = pos_;
  std::vector<uint8_t> h;
  le32(h, 0x04034b50);
  le16(h, 20);       // version needed
  le16(h, 0x0800);   // UTF-8 names
  le16(h, 0);        // stored
  le16(h, kDosTime);
  le16(h, kDosDate);
  le32(h, e.crc);
  le32(h, e.size);
  le32(h, e.size);
  le16(h, static_cast<uint32_t>(name.size()));
  le16(h, 0);
  h.insert(h.end(), name.begin(), name.end());
  raw(h.data(), h.size());
  if (!data.empty()) raw(data.data(), data.size());
  entries_.push_back(e);
  return ok_;
}

bool ZipWriter::addFile(const std::string& name, const std::string& sourcePath, std::string* err) {
  std::ifstream in(sourcePath, std::ios::binary);
  if (!in) return fail(err, "cannot read " + sourcePath);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!add(name, d)) return fail(err, "cannot add " + name + " (archive too large?)");
  return true;
}

bool ZipWriter::close(std::string* err) {
  if (!f_) return fail(err, "archive not open");
  const uint32_t cdStart = pos_;
  std::vector<uint8_t> cd;
  for (const auto& e : entries_) {
    le32(cd, 0x02014b50);
    le16(cd, 20);  // made by
    le16(cd, 20);
    le16(cd, 0x0800);
    le16(cd, 0);
    le16(cd, kDosTime);
    le16(cd, kDosDate);
    le32(cd, e.crc);
    le32(cd, e.size);
    le32(cd, e.size);
    le16(cd, static_cast<uint32_t>(e.name.size()));
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le32(cd, 0);
    le32(cd, e.offset);
    cd.insert(cd.end(), e.name.begin(), e.name.end());
  }
  raw(cd.data(), cd.size());
  std::vector<uint8_t> end;
  le32(end, 0x06054b50);
  le16(end, 0);
  le16(end, 0);
  le16(end, static_cast<uint32_t>(entries_.size()));
  le16(end, static_cast<uint32_t>(entries_.size()));
  le32(end, static_cast<uint32_t>(cd.size()));
  le32(end, cdStart);
  le16(end, 0);
  raw(end.data(), end.size());
  const bool ok = ok_ && std::fclose(f_) == 0;
  f_ = nullptr;
  return ok || fail(err, "write failed");
}

// ---------------------------------------------------------------- reader

namespace {

bool readAll(const std::string& path, std::vector<uint8_t>& d) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

bool readZipDirectory(const std::string& path, std::vector<ZipEntry>& out, std::string* err) {
  std::vector<uint8_t> d;
  if (!readAll(path, d)) return fail(err, "cannot open " + path);
  out.clear();
  if (d.size() < 22) return fail(err, "not a ZIP file");
  size_t eocd = std::string::npos;
  for (size_t p = d.size() - 22 + 1; p-- > 0 && d.size() - p <= 22 + 65535;)
    if (rd32(&d[p]) == 0x06054b50) {
      eocd = p;
      break;
    }
  if (eocd == std::string::npos) return fail(err, "not a ZIP file (no directory)");
  const uint32_t count = rd16(&d[eocd + 10]);
  size_t p = rd32(&d[eocd + 16]);
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 46 > d.size() || rd32(&d[p]) != 0x02014b50) return fail(err, "damaged ZIP directory");
    ZipEntry e;
    e.method = rd16(&d[p + 10]);
    e.crc = rd32(&d[p + 16]);
    e.packed = rd32(&d[p + 20]);
    e.size = rd32(&d[p + 24]);
    const uint16_t nl = rd16(&d[p + 28]), xl = rd16(&d[p + 30]), cl = rd16(&d[p + 32]);
    e.offset = rd32(&d[p + 42]);
    if (p + 46 + nl > d.size()) return fail(err, "damaged ZIP directory");
    e.name.assign(reinterpret_cast<const char*>(&d[p + 46]), nl);
    for (char& c : e.name)
      if (c == '\\') c = '/';
    out.push_back(e);
    p += 46 + nl + xl + cl;
  }
  return true;
}

bool readZipEntry(const std::string& path, const ZipEntry& e, std::vector<uint8_t>& out, std::string* err) {
  std::vector<uint8_t> d;
  if (!readAll(path, d)) return fail(err, "cannot open " + path);
  const size_t p = e.offset;
  if (p + 30 > d.size() || rd32(&d[p]) != 0x04034b50) return fail(err, "damaged entry " + e.name);
  const size_t data = p + 30 + rd16(&d[p + 26]) + rd16(&d[p + 28]);
  if (data + e.packed > d.size()) return fail(err, "truncated entry " + e.name);
  if (e.method == 0) {
    out.assign(d.begin() + static_cast<long>(data), d.begin() + static_cast<long>(data + e.packed));
  } else if (e.method == 8) {
    if (!inflateRaw(&d[data], e.packed, out, e.size)) return fail(err, "cannot decompress " + e.name);
  } else {
    return fail(err, "unsupported compression in " + e.name);
  }
  if (out.size() != e.size || crc32(out.data(), out.size()) != e.crc) return fail(err, "checksum mismatch in " + e.name);
  return true;
}

bool extractZip(const std::string& path, const std::string& dir, std::vector<std::string>* files, std::string* err) {
  std::vector<ZipEntry> entries;
  if (!readZipDirectory(path, entries, err)) return false;
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::path(dir), ec);
  fs::create_directories(root, ec);
  for (const auto& e : entries) {
    const fs::path rel = fs::path(e.name).lexically_normal();
    if (rel.is_absolute() || rel.empty() || *rel.begin() == "..") return fail(err, "unsafe path in archive: " + e.name);
    const fs::path dst = root / rel;
    if (e.dir()) {
      fs::create_directories(dst, ec);
      continue;
    }
    std::vector<uint8_t> data;
    if (!readZipEntry(path, e, data, err)) return false;
    fs::create_directories(dst.parent_path(), ec);
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    if (!o) return fail(err, "cannot write " + dst.string());
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!o) return fail(err, "cannot write " + dst.string());
    if (files) files->push_back(dst.string());
  }
  return true;
}

// ---------------------------------------------------------------- inflate (RFC 1951)

namespace {

struct BitIn {
  const uint8_t* p;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool bad = false;
  int bits(int need) {
    while (cnt < need) {
      if (pos >= n) {
        bad = true;
        return 0;
      }
      buf |= static_cast<uint32_t>(p[pos++]) << cnt;
      cnt += 8;
    }
    const int v = static_cast<int>(buf & ((1u << need) - 1));
    buf >>= need;
    cnt -= need;
    return v;
  }
};

// Canonical Huffman table: number of codes per length and symbols in code order.
struct Huff {
  short count[16] = {};
  short symbol[320] = {};
};

// Returns 0 on success, < 0 for an over-subscribed / incomplete set (incomplete is allowed for single codes).
int build(Huff& h, const short* lengths, int n) {
  for (auto& c : h.count) c = 0;
  for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return -1;
  }
  short offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
  for (int s = 0; s < n; ++s)
    if (lengths[s]) h.symbol[offs[lengths[s]]++] = static_cast<short>(s);
  return left;
}

int decode(BitIn& b, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    code |= b.bits(1);
    if (b.bad) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

constexpr short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                 6145, 8193, 12289, 16385, 24577};
constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitIn& b, std::vector<uint8_t>& out, const Huff& lit, const Huff& dist) {
  for (;;) {
    int sym = decode(b, lit);
    if (sym < 0) return false;
    if (sym < 256) {
      out.push_back(static_cast<uint8_t>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = kLenBase[sym] + b.bits(kLenExtra[sym]);
      const int ds = decode(b, dist);
      if (ds < 0 || ds >= 30) return false;
      const size_t d = static_cast<size_t>(kDistBase[ds] + b.bits(kDistExtra[ds]));
      if (b.bad || d > out.size()) return false;
      const size_t from = out.size() - d;
      for (int k = 0; k < len; ++k) out.push_back(out[from + static_cast<size_t>(k)]);
    }
  }
}

}  // namespace

bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected) {
  out.clear();
  if (expected) out.reserve(expected);
  BitIn b{in, n};
  int last = 0;
  do {
    last = b.bits(1);
    const int type = b.bits(2);
    if (b.bad) return false;
    if (type == 0) {  // stored
      b.buf = 0;
      b.cnt = 0;
      if (b.pos + 4 > n) return false;
      const unsigned len = in[b.pos] | (in[b.pos + 1] << 8), nlen = in[b.pos + 2] | (in[b.pos + 3] << 8);
      b.pos += 4;
      if (len != (~nlen & 0xFFFFu) || b.pos + len > n) return false;
      out.insert(out.end(), in + b.pos, in + b.pos + len);
      b.pos += len;
    } else if (type == 1) {  // fixed codes
      static Huff lit, dist;
      static bool init = [] {
        short l[288];
        for (int s = 0; s < 144; ++s) l[s] = 8;
        for (int s = 144; s < 256; ++s) l[s] = 9;
        for (int s = 256; s < 280; ++s) l[s] = 7;
        for (int s = 280; s < 288; ++s) l[s] = 8;
        build(lit, l, 288);
        short dl[30];
        for (short& v : dl) v = 5;
        build(dist, dl, 30);
        return true;
      }();
      (void)init;
      if (!codes(b, out, lit, dist)) return false;
    } else if (type == 2) {  // dynamic codes
      const int nlen = b.bits(5) + 257, ndist = b.bits(5) + 1, ncode = b.bits(4) + 4;
      if (b.bad || nlen > 286 || ndist > 30) return false;
      static constexpr short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      short lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(b.bits(3));
      Huff lc;
      if (build(lc, lengths, 19) != 0) return false;
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = decode(b, lc);
        if (sym < 0) return false;
        if (sym < 16) {
          lengths[idx++] = static_cast<short>(sym);
        } else {
          short val = 0;
          int rep = 0;
          if (sym == 16) {
            if (idx == 0) return false;
            val = lengths[idx - 1];
            rep = 3 + b.bits(2);
          } else if (sym == 17) {
            rep = 3 + b.bits(3);
          } else {
            rep = 11 + b.bits(7);
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) lengths[idx++] = val;
        }
      }
      if (lengths[256] == 0) return false;
      Huff lit, dist;
      const int el = build(lit, lengths, nlen);
      if (el < 0 || (el > 0 && nlen - lit.count[0] != 1)) return false;
      const int ed = build(dist, lengths + nlen, ndist);
      if (ed < 0 || (ed > 0 && ndist - dist.count[0] != 1)) return false;
      if (!codes(b, out, lit, dist)) return false;
    } else {
      return false;
    }
  } while (!last);
  return true;
}

}  // namespace gl

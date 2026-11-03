// Gridline — ZIP archives. See Zip.h.
#include "Zip.h"
#include "Project.h"
#include "Platform.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

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

void le64(std::vector<uint8_t>& b, uint64_t v) {
  le32(b, static_cast<uint32_t>(v));
  le32(b, static_cast<uint32_t>(v >> 32));
}

// A field that does not fit the classic records holds this value; the figure is in the ZIP64 records.
constexpr uint64_t kOver32 = 0xFFFFFFFFu;
constexpr size_t kOver16 = 0xFFFF;

uint32_t cap32(uint64_t v) { return v >= kOver32 ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

// Past 2 GB fseek's long is not enough everywhere.
bool seekTo(std::FILE* f, uint64_t pos) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0;
#else
  return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

// DOS date/time of 2026-01-01 00:00 (entries carry no meaningful time).
constexpr uint16_t kDosTime = 0, kDosDate = ((2026 - 1980) << 9) | (1 << 5) | 1;

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
  // Eight bytes at a time: packing a large sample should not wait on this.
  struct Tables {
    uint32_t t[8][256];
    Tables() {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
      }
      for (uint32_t i = 0; i < 256; ++i)
        for (int k = 1; k < 8; ++k) t[k][i] = t[0][t[k - 1][i] & 0xFF] ^ (t[k - 1][i] >> 8);
    }
  };
  static const Tables tables;
  const auto& t = tables.t;
  crc = ~crc;
  for (; n >= 8; n -= 8, data += 8) {
    const uint32_t lo = crc ^ rd32(data), hi = rd32(data + 4);
    crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^ t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^
          t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
  }
  for (size_t i = 0; i < n; ++i) crc = t[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---------------------------------------------------------------- writer

ZipWriter::~ZipWriter() {
  if (f_) {  // never closed: the temporary archive goes, the target stays as it was
    std::fclose(f_);
    std::remove(tmp_.c_str());
  }
}

bool ZipWriter::open(const std::string& path, std::string* err) {
  if (f_) {
    std::fclose(f_);
    std::remove(tmp_.c_str());
    f_ = nullptr;
  }
  if (!checkWritableTarget(path, err)) return false;
  static std::atomic<unsigned> serial{0};
  path_ = path;
  tmp_ = path + ".tmp." + std::to_string(static_cast<long>(platform::processId())) + "." + std::to_string(serial.fetch_add(1));
  errno = 0;
  f_ = std::fopen(tmp_.c_str(), "wb");
  if (!f_) return fail(err, fileWriteError(path, errno));
  entries_.clear();
  pos_ = 0;
  errno_ = 0;
  ok_ = true;
  return true;
}

void ZipWriter::raw(const void* p, size_t n) {
  errno = 0;
  if (!f_ || std::fwrite(p, 1, n, f_) != n) {
    if (ok_) errno_ = errno;
    ok_ = false;
  }
  pos_ += n;
}

void ZipWriter::localHeader(const std::string& name, uint64_t size, uint32_t crc) {
  Entry e;
  e.name = name;
  e.crc = crc;
  e.size = size;
  e.offset = pos_;
  const bool big = size >= kOver32;
  std::vector<uint8_t> h;
  le32(h, 0x04034b50);
  le16(h, big ? 45 : 20);  // version needed
  le16(h, 0x0800);         // UTF-8 names
  le16(h, 0);              // stored
  le16(h, kDosTime);
  le16(h, kDosDate);
  le32(h, e.crc);
  le32(h, cap32(size));
  le32(h, cap32(size));
  le16(h, static_cast<uint32_t>(name.size()));
  le16(h, big ? 20 : 0);
  h.insert(h.end(), name.begin(), name.end());
  if (big) {
    le16(h, 0x0001);
    le16(h, 16);
    le64(h, size);
    le64(h, size);
  }
  raw(h.data(), h.size());
  entries_.push_back(std::move(e));
}

bool ZipWriter::add(const std::string& name, const std::vector<uint8_t>& data) {
  if (!f_) return ok_ = false;
  localHeader(name, data.size(), crc32(data.data(), data.size()));
  if (!data.empty()) raw(data.data(), data.size());
  return ok_;
}

bool ZipWriter::addFile(const std::string& name, const std::string& sourcePath, std::string* err) {
  if (!f_) return fail(err, "archive not open");
  errno = 0;
  std::ifstream in(fs::u8path(sourcePath), std::ios::binary);
  if (!in)
    return fail(err, "cannot read " + sourcePath + " to pack it (" + (errno ? std::string(std::strerror(errno)) : std::string("unreadable")) +
                         "). Check the file, or relink it (File > Locate missing samples).");
  // A file on a drive that stopped answering, or one that changes while it is read: never half a
  // sample. The writer is marked failed, so this archive can no longer replace the target.
  auto unreadable = [&] {
    ok_ = false;
    return fail(err, "cannot read all of " + sourcePath + " to pack it. Check the file, or relink it (File > Locate missing samples).");
  };
  std::error_code sizeEc;
  const uint64_t size = fs::file_size(fs::u8path(sourcePath), sizeEc);
  if (sizeEc) return unreadable();
  // Streamed, so a sample needs a fixed amount of memory whatever its size. The checksum is only
  // known at the end: it goes back into the header then.
  localHeader(name, size, 0);
  const uint64_t crcAt = entries_.back().offset + 14;
  static constexpr size_t kChunk = 1u << 20;
  std::vector<char> buf(kChunk);
  uint32_t crc = 0;
  for (uint64_t left = size; left > 0;) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(left, kChunk));
    in.read(buf.data(), static_cast<std::streamsize>(want));
    if (static_cast<size_t>(in.gcount()) != want) return unreadable();
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    crc = crc32(bytes, want, crc);
    // A run of silence stays a hole where the file system can (it reads back as zeros).
    if (bytes[0] == 0 && std::memcmp(bytes, bytes + 1, want - 1) == 0) {
      pos_ += want;
      if (!seekTo(f_, pos_)) {
        errno_ = errno;
        ok_ = false;
      }
    } else {
      raw(buf.data(), want);
    }
    if (!ok_) return fail(err, fileWriteError(path_, errno_));
    left -= want;
    if (chunkHook_ && !chunkHook_(want)) {
      ok_ = false;
      return fail(err, "stopped");
    }
  }
  if (in.peek() != std::char_traits<char>::eof() || in.bad()) return unreadable();
  entries_.back().crc = crc;
  std::vector<uint8_t> patch;
  le32(patch, crc);
  errno = 0;
  if (!seekTo(f_, crcAt) || std::fwrite(patch.data(), 1, patch.size(), f_) != patch.size() || !seekTo(f_, pos_)) {
    errno_ = errno;
    ok_ = false;
    return fail(err, fileWriteError(path_, errno_));
  }
  return true;
}

bool ZipWriter::close(std::string* err) {
  if (!f_) return fail(err, "archive not open");
  const uint64_t cdStart = pos_;
  std::vector<uint8_t> cd;
  for (const auto& e : entries_) {
    const bool bigSize = e.size >= kOver32, bigOffset = e.offset >= kOver32;
    const uint32_t extra = (bigSize ? 16u : 0u) + (bigOffset ? 8u : 0u);
    const uint32_t version = extra ? 45 : 20;
    le32(cd, 0x02014b50);
    le16(cd, version);  // made by
    le16(cd, version);
    le16(cd, 0x0800);
    le16(cd, 0);
    le16(cd, kDosTime);
    le16(cd, kDosDate);
    le32(cd, e.crc);
    le32(cd, cap32(e.size));
    le32(cd, cap32(e.size));
    le16(cd, static_cast<uint32_t>(e.name.size()));
    le16(cd, extra ? extra + 4 : 0);
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le32(cd, 0);
    le32(cd, cap32(e.offset));
    cd.insert(cd.end(), e.name.begin(), e.name.end());
    if (extra) {  // only the fields marked above, in this order
      le16(cd, 0x0001);
      le16(cd, extra);
      if (bigSize) {
        le64(cd, e.size);
        le64(cd, e.size);
      }
      if (bigOffset) le64(cd, e.offset);
    }
  }
  raw(cd.data(), cd.size());
  std::vector<uint8_t> end;
  if (entries_.size() >= kOver16 || cd.size() >= kOver32 || cdStart >= kOver32) {
    const uint64_t recordAt = pos_;
    le32(end, 0x06064b50);  // ZIP64 end record
    le64(end, 44);          // its size after this field
    le16(end, 45);
    le16(end, 45);
    le32(end, 0);
    le32(end, 0);
    le64(end, entries_.size());
    le64(end, entries_.size());
    le64(end, cd.size());
    le64(end, cdStart);
    le32(end, 0x07064b50);  // and where to find it
    le32(end, 0);
    le64(end, recordAt);
    le32(end, 1);
  }
  const uint32_t count = static_cast<uint32_t>(std::min(entries_.size(), kOver16));
  le32(end, 0x06054b50);
  le16(end, 0);
  le16(end, 0);
  le16(end, count);
  le16(end, count);
  le32(end, cap32(cd.size()));
  le32(end, cap32(cdStart));
  le16(end, 0);
  raw(end.data(), end.size());
  errno = 0;
  const bool closed = std::fclose(f_) == 0;
  if (ok_ && !closed) errno_ = errno;
  f_ = nullptr;
  if (!ok_ || !closed) {
    std::remove(tmp_.c_str());
    return fail(err, fileWriteError(path_, errno_));
  }
  if (!platform::replaceFile(tmp_, path_)) {
    const int e = errno;
    std::remove(tmp_.c_str());
    return fail(err, fileWriteError(path_, e));
  }
  return true;
}

// ---------------------------------------------------------------- inflate (RFC 1951)
//
// A streaming decoder: bytes come from a pull callback and leave through a sink, with the 32 KB
// history kept in its own buffer, so an entry of any size needs a fixed amount of memory. Codes up
// to kFastBits long decode through a table, longer ones bit by bit.

namespace {

constexpr int kFastBits = 10;
constexpr size_t kWindow = 32768, kOutBuf = 1 << 18;

struct Huff {
  short count[16] = {};
  short symbol[320] = {};
  uint16_t fast[1 << kFastBits] = {};  // (symbol << 4) | length, 0 = longer or not a code
};

// Returns 0 on success, < 0 for an over-subscribed set, > 0 for an incomplete one (allowed for single codes).
int build(Huff& h, const short* lengths, int n) {
  for (auto& c : h.count) c = 0;
  for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
  std::memset(h.fast, 0, sizeof h.fast);
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
  // The table: every bit pattern starting with a short code (DEFLATE sends codes from the top bit,
  // into a buffer read from the low bit, hence the reversal).
  int code = 0, index = 0;
  for (int len = 1; len <= kFastBits; ++len) {
    for (int k = 0; k < h.count[len]; ++k, ++code, ++index) {
      int rev = 0;
      for (int b = 0; b < len; ++b)
        if (code & (1 << b)) rev |= 1 << (len - 1 - b);
      const auto entry = static_cast<uint16_t>((h.symbol[index] << 4) | len);
      for (int fill = rev; fill < (1 << kFastBits); fill += 1 << len) h.fast[fill] = entry;
    }
    code <<= 1;
  }
  return left;
}

constexpr short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                 6145, 8193, 12289, 16385, 24577};
constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// pull(p, n): the next bytes of input, false at the end of the input (or on a read error).
using Pull = std::function<bool(const uint8_t*& p, size_t& n)>;
// sink(p, n): false stops the decoder.
using Sink = std::function<bool(const uint8_t* p, size_t n)>;

class Inflater {
 public:
  Inflater(Pull pull, Sink sink, uint64_t cap) : pull_(std::move(pull)), sink_(std::move(sink)), cap_(cap), out_(new uint8_t[kOutBuf]) {}

  bool run() {
    auto lit = std::make_unique<Huff>(), dist = std::make_unique<Huff>(), lc = std::make_unique<Huff>();
    int last = 0;
    do {
      last = bits(1);
      const int type = bits(2);
      if (bad_) return false;
      if (type == 0) {
        if (!stored()) return false;
      } else if (type == 1) {
        short lengths[320];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        build(*lit, lengths, 288);
        for (s = 0; s < 30; ++s) lengths[s] = 5;
        build(*dist, lengths, 30);
        if (!codes(*lit, *dist)) return false;
      } else if (type == 2) {
        if (!dynamic(*lit, *dist, *lc)) return false;
      } else {
        return false;
      }
    } while (!last);
    return flush(pos_);
  }

 private:
  // Keeps at least `need` bits available when the input has them (fewer only at its end).
  void fill(int need) {
    while (cnt_ < need) {
      if (at_ == end_) {
        const uint8_t* p = nullptr;
        size_t n = 0;
        if (!pull_(p, n) || n == 0) return;
        at_ = p;
        end_ = p + n;
      }
      buf_ |= static_cast<uint64_t>(*at_++) << cnt_;
      cnt_ += 8;
    }
  }
  int bits(int need) {
    if (need == 0) return 0;
    fill(need);
    if (cnt_ < need) {
      bad_ = true;
      return 0;
    }
    const int v = static_cast<int>(buf_ & ((uint64_t{1} << need) - 1));
    buf_ >>= need;
    cnt_ -= need;
    return v;
  }
  int decode(const Huff& h) {
    fill(15);
    const uint16_t e = h.fast[buf_ & ((1u << kFastBits) - 1)];
    if (e) {
      const int len = e & 15;
      if (len > cnt_) {
        bad_ = true;
        return -1;
      }
      buf_ >>= len;
      cnt_ -= len;
      return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= bits(1);
      if (bad_) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
  // Hands over everything before `upto`.
  bool flush(size_t upto) {
    if (upto > sent_ && !sink_(out_.get() + sent_, upto - sent_)) return false;
    sent_ = upto;
    return true;
  }
  // Room for a whole match: when the buffer is nearly full, it is handed over and the history
  // moves to the front.
  bool room() {
    if (pos_ + 512 < kOutBuf) return true;
    if (!flush(pos_)) return false;
    std::memmove(out_.get(), out_.get() + pos_ - kWindow, kWindow);
    pos_ = sent_ = kWindow;
    return true;
  }
  bool stored() {
    const int drop = cnt_ & 7;  // to the next byte
    buf_ >>= drop;
    cnt_ -= drop;
    const int len = bits(16), nlen = bits(16);
    if (bad_ || len != (~nlen & 0xFFFF)) return false;
    total_ += static_cast<uint64_t>(len);
    if (total_ > cap_) return false;
    for (int k = 0; k < len; ++k) {
      const int b = bits(8);
      if (bad_ || !room()) return false;
      out_[pos_++] = static_cast<uint8_t>(b);
    }
    return true;
  }
  bool codes(const Huff& lit, const Huff& dist) {
    for (;;) {
      if (!room()) return false;
      int sym = decode(lit);
      if (sym < 0) return false;
      if (sym < 256) {
        if (++total_ > cap_) return false;  // more than the entry says it holds (J-36: inflate bombs)
        out_[pos_++] = static_cast<uint8_t>(sym);
      } else if (sym == 256) {
        return true;
      } else {
        sym -= 257;
        if (sym >= 29) return false;
        const int len = kLenBase[sym] + bits(kLenExtra[sym]);
        const int ds = decode(dist);
        if (ds < 0 || ds >= 30) return false;
        const size_t d = static_cast<size_t>(kDistBase[ds] + bits(kDistExtra[ds]));
        if (bad_ || d > total_ || d > pos_) return false;
        total_ += static_cast<uint64_t>(len);
        if (total_ > cap_) return false;
        const uint8_t* from = out_.get() + pos_ - d;
        uint8_t* to = out_.get() + pos_;
        for (int k = 0; k < len; ++k) to[k] = from[k];
        pos_ += static_cast<size_t>(len);
      }
    }
  }
  bool dynamic(Huff& lit, Huff& dist, Huff& lc) {
    const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
    if (bad_ || nlen > 286 || ndist > 30) return false;
    static constexpr short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(bits(3));
    if (bad_ || build(lc, lengths, 19) != 0) return false;
    std::memset(lengths, 0, sizeof lengths);
    int idx = 0;
    while (idx < nlen + ndist) {
      const int sym = decode(lc);
      if (sym < 0) return false;
      if (sym < 16) {
        lengths[idx++] = static_cast<short>(sym);
      } else {
        short val = 0;
        int rep = 0;
        if (sym == 16) {
          if (idx == 0) return false;
          val = lengths[idx - 1];
          rep = 3 + bits(2);
        } else if (sym == 17) {
          rep = 3 + bits(3);
        } else {
          rep = 11 + bits(7);
        }
        if (bad_ || idx + rep > nlen + ndist) return false;
        while (rep--) lengths[idx++] = val;
      }
    }
    if (lengths[256] == 0) return false;
    const int el = build(lit, lengths, nlen);
    if (el < 0 || (el > 0 && nlen - lit.count[0] != 1)) return false;
    const int ed = build(dist, lengths + nlen, ndist);
    if (ed < 0 || (ed > 0 && ndist - dist.count[0] != 1)) return false;
    return codes(lit, dist);
  }

  Pull pull_;
  Sink sink_;
  uint64_t cap_;
  std::unique_ptr<uint8_t[]> out_;
  size_t pos_ = 0, sent_ = 0;
  uint64_t total_ = 0;
  const uint8_t* at_ = nullptr;
  const uint8_t* end_ = nullptr;
  uint64_t buf_ = 0;
  int cnt_ = 0;
  bool bad_ = false;
};

}  // namespace

bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected) {
  out.clear();
  // `expected` comes from the archive: never reserve more than deflate can
  // expand `n` bytes to (1032:1), and stop once the output passes it (J-36).
  if (expected) out.reserve(std::min(expected, n * 1032 + 1024));
  const uint64_t cap = expected ? expected : ~uint64_t{0};
  bool given = false;
  Inflater inf(
      [&](const uint8_t*& p, size_t& got) {
        if (given) return false;
        given = true;
        p = in;
        got = n;
        return true;
      },
      [&](const uint8_t* p, size_t m) {
        out.insert(out.end(), p, p + m);
        return true;
      },
      cap);
  return inf.run();
}

// ---------------------------------------------------------------- reader

namespace {

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const size_t more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
    if (more == 9 || i + more >= s.size()) {
      if (more != 0) return false;
    }
    for (size_t k = 1; k <= more; ++k)
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    i += more + 1;
  }
  return true;
}

// A name in the archive's old code page (the ZIP default, used by Windows tools).
std::string fromCp437(const std::string& s) {
  static constexpr uint16_t kHigh[128] = {
      0xC7,   0xFC,   0xE9,   0xE2,   0xE4,   0xE0,   0xE5,   0xE7,   0xEA,   0xEB,   0xE8,   0xEF,   0xEE,   0xEC,   0xC4,   0xC5,
      0xC9,   0xE6,   0xC6,   0xF4,   0xF6,   0xF2,   0xFB,   0xF9,   0xFF,   0xD6,   0xDC,   0xA2,   0xA3,   0xA5,   0x20A7, 0x192,
      0xE1,   0xED,   0xF3,   0xFA,   0xF1,   0xD1,   0xAA,   0xBA,   0xBF,   0x2310, 0xAC,   0xBD,   0xBC,   0xA1,   0xAB,   0xBB,
      0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
      0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
      0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
      0x3B1,  0xDF,   0x393,  0x3C0,  0x3A3,  0x3C3,  0xB5,   0x3C4,  0x3A6,  0x398,  0x3A9,  0x3B4,  0x221E, 0x3C6,  0x3B5,  0x2229,
      0x2261, 0xB1,   0x2265, 0x2264, 0x2320, 0x2321, 0xF7,   0x2248, 0xB0,   0x2219, 0xB7,   0x221A, 0x207F, 0xB2,   0x25A0, 0xA0};
  std::string o;
  o.reserve(s.size() + 8);
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    const unsigned u = c < 0x80 ? c : kHigh[c - 0x80];
    if (u < 0x80) {
      o += static_cast<char>(u);
    } else if (u < 0x800) {
      o += static_cast<char>(0xC0 | (u >> 6));
      o += static_cast<char>(0x80 | (u & 0x3F));
    } else {
      o += static_cast<char>(0xE0 | (u >> 12));
      o += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (u & 0x3F));
    }
  }
  return o;
}

uint64_t rd64(const uint8_t* p) { return rd32(p) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }

// Exactly `n` bytes at `at`.
bool readExact(remotefs::ByteSource& src, uint64_t at, uint8_t* out, size_t n, std::string* err) {
  size_t done = 0;
  while (done < n) {
    const int64_t got = src.readAt(at + done, out + done, n - done, err);
    if (got < 0) return false;
    if (got == 0) return fail(err, "the file ended early: it may not be fully downloaded");
    done += static_cast<size_t>(got);
  }
  return true;
}

constexpr uint64_t kMaxDirectory = 256u * 1024u * 1024u;  // a central directory beyond this is not a project

}  // namespace

bool readZipDirectory(remotefs::ByteSource& src, std::vector<ZipEntry>& out, std::string* err) {
  out.clear();
  const uint64_t size = static_cast<uint64_t>(std::max<int64_t>(src.size(), 0));
  if (size < 22) return fail(err, "not a ZIP file");
  // The end record sits in the last 64 KB.
  const size_t tailN = static_cast<size_t>(std::min<uint64_t>(size, 22 + 65535));
  const uint64_t tailAt = size - tailN;
  std::vector<uint8_t> tail(tailN);
  if (!readExact(src, tailAt, tail.data(), tailN, err)) return false;
  size_t eocd = std::string::npos;
  for (size_t p = tailN - 22 + 1; p-- > 0;)
    if (rd32(&tail[p]) == 0x06054b50) {
      eocd = p;
      break;
    }
  if (eocd == std::string::npos) return fail(err, "not a ZIP file (no directory)");
  uint64_t count = rd16(&tail[eocd + 10]), cdSize = rd32(&tail[eocd + 12]), cdAt = rd32(&tail[eocd + 16]);
  if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdAt == 0xFFFFFFFFu) {  // ZIP64: the real figures are in the records before it
    uint8_t loc[20], rec[56];
    const uint64_t locAt = tailAt + eocd;
    if (locAt < 20 || !readExact(src, locAt - 20, loc, sizeof loc, err) || rd32(loc) != 0x07064b50) return fail(err, "damaged ZIP directory");
    const uint64_t recAt = rd64(loc + 8);
    if (recAt > size - sizeof rec || !readExact(src, recAt, rec, sizeof rec, err) || rd32(rec) != 0x06064b50) return fail(err, "damaged ZIP directory");
    count = rd64(rec + 32);
    cdSize = rd64(rec + 40);
    cdAt = rd64(rec + 48);
  }
  if (count == 0) return true;
  if (cdSize > kMaxDirectory || cdAt > size || cdSize > size - cdAt) return fail(err, "damaged ZIP directory");
  std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
  if (!readExact(src, cdAt, cd.data(), cd.size(), err)) return false;
  size_t p = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return fail(err, "damaged ZIP directory");
    ZipEntry e;
    const uint16_t flags = rd16(&cd[p + 8]);
    e.encrypted = (flags & 1) != 0;
    e.method = rd16(&cd[p + 10]);
    e.crc = rd32(&cd[p + 16]);
    e.packed = rd32(&cd[p + 20]);
    e.size = rd32(&cd[p + 24]);
    const size_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
    e.offset = rd32(&cd[p + 42]);
    if (p + 46 + nl + xl + cl > cd.size()) return fail(err, "damaged ZIP directory");
    e.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), nl);
    // Extra fields: ZIP64 sizes (only those marked in the record, in this order), the UTF-8 name.
    bool nameDone = (flags & 0x800) != 0 && validUtf8(e.name);
    for (size_t x = p + 46 + nl, xe = x + xl; x + 4 <= xe;) {
      const uint16_t id = rd16(&cd[x]), len = rd16(&cd[x + 2]);
      const size_t body = x + 4;
      if (body + len > xe) break;
      if (id == 0x0001) {
        size_t at = body;
        auto next = [&](uint64_t& v) {
          if (v != 0xFFFFFFFFu || at + 8 > body + len) return;
          v = rd64(&cd[at]);
          at += 8;
        };
        next(e.size);
        next(e.packed);
        next(e.offset);
      } else if (id == 0x7075 && len > 5 && cd[body] == 1 && !nameDone) {
        if (rd32(&cd[body + 1]) == crc32(cd.data() + p + 46, nl)) {
          std::string u(reinterpret_cast<const char*>(&cd[body + 5]), len - 5u);
          if (validUtf8(u)) {
            e.name = std::move(u);
            nameDone = true;
          }
        }
      }
      x = body + len;
    }
    if (!nameDone && !validUtf8(e.name)) e.name = fromCp437(e.name);
    for (char& c : e.name)
      if (c == '\\') c = '/';
    out.push_back(std::move(e));
    p += 46 + nl + xl + cl;
  }
  return true;
}

bool readZipDirectory(const std::string& path, std::vector<ZipEntry>& out, std::string* err) {
  std::string e;
  auto src = remotefs::openFile(path, &e);
  if (!src) return fail(err, "cannot open " + path);
  return readZipDirectory(*src, out, err);
}

namespace {

// Decodes one entry from `src` into `sink`, checking its size and CRC. The input goes through a
// chunk buffer shared by the entries; `w` sees every byte as it is read.
bool streamEntry(remotefs::ByteSource& src, const ZipEntry& e, const remotefs::ReadWatch& w, std::vector<uint8_t>& chunk, const Sink& sink,
                 std::string* err) {
  if (e.encrypted) return fail(err, "this archive is protected with a password: " + e.name);
  if (e.method != 0 && e.method != 8) return fail(err, "unsupported compression in " + e.name);
  uint8_t h[30];
  std::string readErr;
  if (!readExact(src, e.offset, h, sizeof h, &readErr)) return fail(err, readErr.rfind("cannot read", 0) == 0 ? readErr : "damaged entry " + e.name);
  if (rd32(h) != 0x04034b50) return fail(err, "damaged entry " + e.name);
  const uint64_t data = e.offset + 30 + rd16(h + 26) + rd16(h + 28);
  const uint64_t fileSize = static_cast<uint64_t>(std::max<int64_t>(src.size(), 0));
  if (data > fileSize || e.packed > fileSize - data) return fail(err, "truncated entry " + e.name);
  uint64_t at = data, left = e.packed;
  uint32_t crc = 0;
  uint64_t written = 0;
  bool stopped = false, sinkFailed = false, readFailed = false;
  const auto pull = [&](const uint8_t*& p, size_t& n) {
    if (left == 0) return false;
    if (w.cancelled()) {
      stopped = true;
      return false;
    }
    const size_t want = static_cast<size_t>(std::min<uint64_t>(left, chunk.size()));
    const int64_t got = src.readAt(at, chunk.data(), want, &readErr);
    if (got <= 0) {
      if (got == 0) readErr = "the file ended early: it may not be fully downloaded";
      readFailed = true;
      return false;
    }
    at += static_cast<uint64_t>(got);
    left -= static_cast<uint64_t>(got);
    if (w.onBytes) w.onBytes(static_cast<uint64_t>(got));
    p = chunk.data();
    n = static_cast<size_t>(got);
    return true;
  };
  const auto out = [&](const uint8_t* p, size_t n) {
    crc = crc32(p, n, crc);
    written += n;
    if (!sink(p, n)) {
      sinkFailed = true;
      return false;
    }
    if (w.cancelled()) {
      stopped = true;
      return false;
    }
    return true;
  };
  bool ok = true;
  if (e.method == 0) {
    const uint8_t* p = nullptr;
    size_t n = 0;
    while (ok && pull(p, n)) ok = out(p, n);
  } else {
    Inflater inf(pull, out, e.size);
    ok = inf.run();
  }
  if (stopped) return fail(err, remotefs::kCancelled);
  if (readFailed) return fail(err, readErr);
  if (sinkFailed) return false;  // (the sink's own message)
  if (!ok) return fail(err, "cannot decompress " + e.name);
  if (written != e.size || crc != e.crc) return fail(err, "checksum mismatch in " + e.name);
  return true;
}

}  // namespace

bool readZipEntry(const std::string& path, const ZipEntry& e, std::vector<uint8_t>& out, std::string* err) {
  std::string oe;
  auto src = remotefs::openFile(path, &oe);
  if (!src) return fail(err, "cannot open " + path);
  out.clear();
  // (the size comes from the archive: never reserved beyond what the entry can expand to)
  out.reserve(static_cast<size_t>(std::min<uint64_t>(e.size, e.packed * 1032 + 1024)));
  std::vector<uint8_t> chunk(remotefs::kReadChunk);
  return streamEntry(*src, e, {}, chunk,
                     [&](const uint8_t* p, size_t n) {
                       out.insert(out.end(), p, p + n);
                       return true;
                     },
                     err);
}

bool extractZipEntries(remotefs::ByteSource& src, const std::vector<ZipEntry>& entries, const std::string& dir, std::vector<std::string>* files,
                       const ZipExtractOptions& opt, std::string* err) {
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::u8path(dir), ec);
  // Every name is checked before anything is written.
  std::vector<fs::path> dest(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    fs::path rel;
    try {  // Windows: a name that is not valid UTF-8 cannot become a path
      rel = fs::u8path(entries[i].name).lexically_normal();
    } catch (const std::exception&) {
      return fail(err, "unreadable file name in archive");
    }
    // Absolute, drive-relative (C:x) or UNC names never leave the folder.
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory() || rel.empty() || *rel.begin() == "..")
      return fail(err, "unsafe path in archive: " + entries[i].name);
    dest[i] = root / rel;
  }
  fs::create_directories(root, ec);
  std::vector<size_t> order(entries.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return entries[a].offset < entries[b].offset; });
  std::vector<uint8_t> chunk(remotefs::kReadChunk);
  for (const size_t i : order) {
    const ZipEntry& e = entries[i];
    if (opt.onEntry) opt.onEntry(e, i);
    if (e.dir()) {
      fs::create_directories(dest[i], ec);
      continue;
    }
    fs::create_directories(dest[i].parent_path(), ec);
    const std::string shown = dest[i].u8string();
    errno = 0;
    std::ofstream o(dest[i], std::ios::binary | std::ios::trunc);
    if (!o) return fail(err, fileWriteError(shown, errno));
    bool wrote = true;
    int werr = 0;
    const bool ok = streamEntry(src, e, opt.watch, chunk,
                                [&](const uint8_t* p, size_t n) {
                                  errno = 0;
                                  o.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
                                  wrote = static_cast<bool>(o);
                                  werr = errno;
                                  return wrote;
                                },
                                err);
    if (!wrote) {
      o.close();
      fs::remove(dest[i], ec);
      return fail(err, fileWriteError(shown, werr));
    }
    if (!ok) {
      o.close();
      fs::remove(dest[i], ec);  // never a half-written or damaged file under its real name
      return false;
    }
    errno = 0;
    o.close();
    if (!o) return fail(err, fileWriteError(shown, errno));
  }
  if (files)
    for (size_t i = 0; i < entries.size(); ++i)
      if (!entries[i].dir()) files->push_back(dest[i].u8string());
  return true;
}

bool extractZip(const std::string& path, const std::string& dir, std::vector<std::string>* files, std::string* err) {
  std::string oe;
  auto src = remotefs::openFile(path, &oe);
  if (!src) return fail(err, "cannot open " + path);
  std::vector<ZipEntry> entries;
  if (!readZipDirectory(*src, entries, err)) return false;
  return extractZipEntries(*src, entries, dir, files, {}, err);
}

}  // namespace gl

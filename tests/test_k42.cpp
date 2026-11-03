// Gridline - K-42: opening projects from mounted web and network drives, on made-up archives and a
// counting, stalling, failing reader:
//  * a zip is read once (directory from the end, entries in file order), never per entry;
//  * the streaming inflater decodes stored, fixed and dynamic blocks, across its buffer, a byte at a
//    time, and refuses what it must; ZIP64 and old code page names read;
//  * the project is picked from one, several, backups and macOS leftovers;
//  * an archive on a drive unpacks to the local cache, nothing beside it, and is reused unread; the
//    last copy opens when the drive is offline; cancel and read errors leave nothing behind;
//  * the loader: progress in bytes, the interface never waits on a stalled read, cancel returns at
//    once, a read error says why, several projects ask, bare project files read the same way;
//  * nothing in an open walks a folder (counted), the browser index and the relink search do not walk
//    a drive, autosave's and backups' folder stay local, the mount table decides without the drive.
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/Backup.h"
#include "../src/core/BrowserIndex.h"
#include "../src/core/Launcher.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectArchive.h"
#include "../src/core/ProjectFiles.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/Recents.h"
#include "../src/core/RemoteFs.h"
#include "../src/core/WavWriter.h"
#include "../src/core/Zip.h"
#include "../src/core/flp/FlpPackage.h"
#include "../src/engine/ProjectLoad.h"
#include "../src/engine/Sample.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;
using projload::Loader;
using projload::Phase;

namespace {
void expect(bool ok, const std::string& what) { expect(ok, what.c_str()); }
}  // namespace

namespace {

using Bytes = std::vector<uint8_t>;

void le16(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void le32(Bytes& b, uint32_t v) {
  le16(b, v & 0xFFFF);
  le16(b, v >> 16);
}
void le64(Bytes& b, uint64_t v) {
  le32(b, static_cast<uint32_t>(v));
  le32(b, static_cast<uint32_t>(v >> 32));
}

// ---- a small fixed-block DEFLATE encoder, for the inflater's streams

struct BitOut {
  Bytes out;
  uint32_t acc = 0;
  int n = 0;
  void bits(uint32_t v, int count) {
    acc |= v << n;
    n += count;
    while (n >= 8) {
      out.push_back(static_cast<uint8_t>(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void huff(uint32_t code, int count) {  // Huffman codes go from the top bit
    for (int i = count - 1; i >= 0; --i) bits((code >> i) & 1, 1);
  }
  void flushByte() {
    if (n) bits(0, 8 - n);
  }
  void lit(int s) {
    if (s < 144) huff(0x30u + static_cast<uint32_t>(s), 8);
    else if (s < 256) huff(0x190u + static_cast<uint32_t>(s - 144), 9);
    else if (s < 280) huff(static_cast<uint32_t>(s - 256), 7);
    else huff(0xC0u + static_cast<uint32_t>(s - 280), 8);
  }
  void match258(int dist) {  // length 258 (symbol 285), a distance of 32768 at most
    lit(285);
    static const int base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int c = 29;
    while (base[c] > dist) --c;
    huff(static_cast<uint32_t>(c), 5);
    bits(static_cast<uint32_t>(dist - base[c]), extra[c]);
  }
};

// `data` as: `seed` literals, then copies from far back where they fit, in fixed blocks, with a
// stored block in the middle.
Bytes deflateFixed(const Bytes& data, int dist = 0) {
  BitOut b;
  size_t i = 0;
  const size_t half = data.size() / 2;
  b.bits(0, 1);  // first block, fixed
  b.bits(1, 2);
  while (i < half) {
    size_t same = 0;
    if (dist > 0 && i >= static_cast<size_t>(dist))
      while (same < 258 && i + same < half && data[i + same] == data[i + same - static_cast<size_t>(dist)]) ++same;
    if (same == 258) {
      b.match258(dist);
      i += 258;
    } else {
      b.lit(data[i++]);
    }
  }
  b.lit(256);
  // A stored block (short enough), then the rest in a last fixed block.
  const size_t storedN = std::min<size_t>(data.size() - i, 1000);
  b.bits(0, 1);
  b.bits(0, 2);
  b.flushByte();
  b.bits(static_cast<uint32_t>(storedN), 16);
  b.bits(static_cast<uint32_t>(~storedN & 0xFFFF), 16);
  for (size_t k = 0; k < storedN; ++k) b.bits(data[i++], 8);
  b.bits(1, 1);
  b.bits(1, 2);
  while (i < data.size()) b.lit(data[i++]);
  b.lit(256);
  b.flushByte();
  return b.out;
}

// ---- a zip in memory

struct Item {
  std::string name;
  Bytes data;
  bool deflate = false;
  uint16_t flags = 0x0800;
};

void put(Bytes& to, const std::string& s) { to.insert(to.end(), s.begin(), s.end()); }
Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes makeZip(const std::vector<Item>& items) {
  Bytes z, cd;
  for (const auto& it : items) {
    const Bytes packed = it.deflate ? deflateFixed(it.data) : it.data;
    const uint32_t crc = crc32(it.data.data(), it.data.size());
    const auto at = static_cast<uint32_t>(z.size());
    le32(z, 0x04034b50);
    le16(z, 20);
    le16(z, it.flags);
    le16(z, it.deflate ? 8 : 0);
    le16(z, 0);
    le16(z, 0);
    le32(z, crc);
    le32(z, static_cast<uint32_t>(packed.size()));
    le32(z, static_cast<uint32_t>(it.data.size()));
    le16(z, static_cast<uint32_t>(it.name.size()));
    le16(z, 0);
    put(z, it.name);
    z.insert(z.end(), packed.begin(), packed.end());
    le32(cd, 0x02014b50);
    le16(cd, 20);
    le16(cd, 20);
    le16(cd, it.flags);
    le16(cd, it.deflate ? 8 : 0);
    le16(cd, 0);
    le16(cd, 0);
    le32(cd, crc);
    le32(cd, static_cast<uint32_t>(packed.size()));
    le32(cd, static_cast<uint32_t>(it.data.size()));
    le16(cd, static_cast<uint32_t>(it.name.size()));
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le32(cd, 0);
    le32(cd, at);
    put(cd, it.name);
  }
  const auto cdAt = static_cast<uint32_t>(z.size());
  z.insert(z.end(), cd.begin(), cd.end());
  le32(z, 0x06054b50);
  le16(z, 0);
  le16(z, 0);
  le16(z, static_cast<uint32_t>(items.size()));
  le16(z, static_cast<uint32_t>(items.size()));
  le32(z, static_cast<uint32_t>(cd.size()));
  le32(z, cdAt);
  le16(z, 0);
  return z;
}

bool writeBytes(const fs::path& p, const Bytes& b) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return static_cast<bool>(o);
}

// ---- readers: counted, slow, stalling, failing

struct Shared {
  std::atomic<uint64_t> bytes{0}, reads{0};
  std::atomic<int> opens{0};
  std::atomic<bool> released{true};  // false: every read blocks until it is true
  std::atomic<bool> blocked{false};
  std::atomic<int64_t> failAt{-1};   // reads reaching this offset fail with EIO
  int chunkDelayMs = 0;
  std::mutex mu;
  std::condition_variable cv;
  void release() {
    {
      std::lock_guard<std::mutex> lk(mu);
      released = true;
    }
    cv.notify_all();
  }
};

class TestSource final : public remotefs::ByteSource {
 public:
  TestSource(Bytes b, std::shared_ptr<Shared> s, std::string path) : b_(std::move(b)), s_(std::move(s)), path_(std::move(path)) {}
  int64_t size() const override { return static_cast<int64_t>(b_.size()); }
  int64_t readAt(uint64_t offset, void* out, size_t n, std::string* err) override {
    if (!s_->released.load()) {
      s_->blocked = true;
      std::unique_lock<std::mutex> lk(s_->mu);
      s_->cv.wait(lk, [&] { return s_->released.load(); });
      s_->blocked = false;
    }
    if (s_->chunkDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(s_->chunkDelayMs));
    if (s_->failAt >= 0 && static_cast<int64_t>(offset + n) > s_->failAt) {
      if (err) *err = remotefs::readError(path_, EIO);
      return -1;
    }
    if (offset >= b_.size()) return 0;
    const size_t m = std::min(n, b_.size() - static_cast<size_t>(offset));
    std::memcpy(out, b_.data() + offset, m);
    s_->bytes += m;
    s_->reads += 1;
    return static_cast<int64_t>(m);
  }

 private:
  Bytes b_;
  std::shared_ptr<Shared> s_;
  std::string path_;
};

using OpenFn = std::function<std::unique_ptr<remotefs::ByteSource>(const std::string&, std::string*)>;
OpenFn opener(const Bytes& z, std::shared_ptr<Shared> s) {
  return [z, s](const std::string& path, std::string*) -> std::unique_ptr<remotefs::ByteSource> {
    s->opens += 1;
    return std::make_unique<TestSource>(z, s, path);
  };
}

std::set<std::string> listing(const fs::path& dir) {
  std::set<std::string> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.insert(it->path().lexically_relative(dir).generic_string());
  return out;
}

Bytes noise(size_t n, uint32_t seed = 12345) {
  Bytes b(n);
  uint32_t x = seed;
  for (auto& v : b) {
    x = x * 1664525u + 1013904223u;
    v = static_cast<uint8_t>(x >> 24);
  }
  return b;
}

std::string writeTone(const fs::path& p) {
  AudioBuffer b(2, 2205);
  for (size_t f = 0; f < b.frames; ++f) b.data[f] = b.data[b.frames + f] = 0.3f * static_cast<float>(std::sin(static_cast<double>(f) * 0.05));
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  writeWav(p.u8string(), b, 44100);
  return p.u8string();
}

Song toneSong(const std::string& wav) {
  Song s;
  Channel c;
  c.id = s.allocateId();
  c.name = "Tone";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = wav;
  c.mixerTrack = s.masterId();
  s.channels.clear();
  s.channels.push_back(c);
  s.info.title = "K42";
  return s;
}

void waitFor(Loader& L, Phase p, int seconds = 30) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (L.phase() != p && std::chrono::steady_clock::now() < until) {
    L.step(0.004);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool logHas(const Loader& L, const std::string& part) {
  for (const auto& l : L.log())
    if (l.text().find(part) != std::string::npos) return true;
  return false;
}

// ---------------------------------------------------------------- the reader

void testZipReadOnce() {
  const size_t entries = 40, each = 200 * 1024;
  std::vector<Item> items;
  for (size_t i = 0; i < entries; ++i) items.push_back({"s" + std::to_string(i) + ".wav", noise(each, static_cast<uint32_t>(i + 1))});
  const Bytes z = makeZip(items);
  auto sh = std::make_shared<Shared>();
  TestSource src(z, sh, "x.zip");
  std::vector<ZipEntry> dir;
  std::string err;
  expect(readZipDirectory(src, dir, &err) && dir.size() == entries, "the directory reads (" + err + ")");
  expect(sh->bytes.load() < 80 * 1024, "the directory reads only the end of the file (" + std::to_string(sh->bytes.load()) + " bytes)");
  const fs::path out = fs::temp_directory_path() / "k42-once";
  std::vector<std::string> files;
  sh->bytes = 0;
  expect(extractZipEntries(src, dir, out.u8string(), &files, {}, &err) && files.size() == entries, "every entry unpacks (" + err + ")");
  // The old reader read the whole archive once per entry: 40 times here.
  expect(sh->bytes.load() <= z.size() + entries * 64, "each byte is read once (" + std::to_string(sh->bytes.load()) + " of " + std::to_string(z.size()) + ")");
  std::ifstream back(files[7], std::ios::binary);
  const Bytes got((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
  expect(got == noise(each, 8), "an entry comes back byte for byte");
  fs::remove_all(out);
}

void testInflate() {
  // Dynamic blocks, from a real compressor (2999 bytes of words, CRC known).
  const char* hex =
      "95565b6ec3300cbb4aae1664c616c0f582341fdbed07c4766a52a2d2fdac9d1f9444917297afb99494a7c7fa93f6699b8f23ed652adf479a3ef7f523af65f8f29c1f5b4ed396e7"
      "dfbc3e8fd7c6d250ecceb5520358cc1eb27f5e1b74e1023207655615e1ace5fc1364a7d1551a2d6addbeea6fb71b9b3a9e21acc19d79cacafb97e15407b2a7b187b79d81d61b3a"
      "5a7a5ca739d70f2c202be0ea0c002b6fd0c3f8354b5ab4356af540628b6b022305649465c7ad246cc85cd6c7204aecd67963fb847d5f7b4823288602bb8403867132b16985e75b"
      "ccedc5a0746e8b62c83f6da4054119930fbb7283c50a477a1e174d3d860f0e8e063774539dda3a7ee5d06c7fc2385aa25cf1c387e4462877e100ba19c3583c6dbe310fe414fcc7"
      "d3c69e24044c0e2c6330d9c5fae9bd6153a80447803f8ab0b3a3f1c4c31a09cbde09adc5536d5c9464a13f4413789ebae1e9922b6ec0f7fe01fdc201210c4841769cfb13911df5"
      "f2668e84ce12efa322d9b73435ca7d4cc3616389a59f5b41d56e206522cc62504830e4d52fa6405fe60a64ac845cd7ff00";
  Bytes dyn;
  for (const char* p = hex; *p; p += 2) {
    const char two[3] = {p[0], p[1], 0};
    dyn.push_back(static_cast<uint8_t>(std::strtoul(two, nullptr, 16)));
  }
  Bytes out;
  expect(inflateRaw(dyn.data(), dyn.size(), out, 2999) && out.size() == 2999 && crc32(out.data(), out.size()) == 611244369u, "a dynamic block decodes");
  expect(!inflateRaw(dyn.data(), dyn.size(), out, 100), "output past the expected size stops the decoder");
  expect(!inflateRaw(dyn.data(), dyn.size() / 2, out, 2999), "a cut stream fails");

  // Fixed and stored blocks, far back copies, and more output than the decoder's buffer.
  Bytes big(3 * 1024 * 1024);
  const Bytes pattern = noise(20000, 99);
  for (size_t i = 0; i < big.size(); ++i) big[i] = pattern[i % pattern.size()];
  const Bytes packed = deflateFixed(big, 20000);
  expect(packed.size() < big.size() * 3 / 4, "the fixture really uses far back copies");
  expect(inflateRaw(packed.data(), packed.size(), out, big.size()) && out == big, "copies cross the decoder's buffer");

  // One byte at a time through a source (a chunk boundary anywhere in a code).
  const Bytes z = makeZip({{"a.bin", Bytes(big.begin(), big.begin() + 300000), true}});
  const fs::path dir = fs::temp_directory_path() / "k42-inflate";
  class OneByte final : public remotefs::ByteSource {
   public:
    explicit OneByte(const Bytes& b) : b_(b) {}
    int64_t size() const override { return static_cast<int64_t>(b_.size()); }
    int64_t readAt(uint64_t at, void* out, size_t n, std::string*) override {
      if (at >= b_.size() || n == 0) return 0;
      *static_cast<uint8_t*>(out) = b_[at];
      return 1;
    }

   private:
    const Bytes& b_;
  } one(z);
  std::vector<ZipEntry> entries;
  std::string err;
  expect(readZipDirectory(one, entries, &err) && extractZipEntries(one, entries, dir.u8string(), nullptr, {}, &err), "a byte at a time (" + err + ")");
  std::ifstream back(dir / "a.bin", std::ios::binary);
  const Bytes got((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
  expect(got.size() == 300000 && std::equal(got.begin(), got.end(), big.begin()), "the same bytes come out");
  fs::remove_all(dir);
}

void testNamesAndZip64() {
  // ZIP64: the records carry the real figures, the sizes in the extra field.
  Bytes z;
  const std::string name = "song.flp";
  const Bytes data = bytesOf("hello");
  const uint32_t crc = crc32(data.data(), data.size());
  le32(z, 0x04034b50);
  le16(z, 45);
  le16(z, 0);
  le16(z, 0);
  le16(z, 0);
  le16(z, 0);
  le32(z, crc);
  le32(z, 5);
  le32(z, 5);
  le16(z, static_cast<uint32_t>(name.size()));
  le16(z, 0);
  put(z, name);
  z.insert(z.end(), data.begin(), data.end());
  const auto cdAt = static_cast<uint64_t>(z.size());
  Bytes cd;
  le32(cd, 0x02014b50);
  le16(cd, 45);
  le16(cd, 45);
  le16(cd, 0);
  le16(cd, 0);
  le16(cd, 0);
  le16(cd, 0);
  le32(cd, crc);
  le32(cd, 0xFFFFFFFFu);
  le32(cd, 0xFFFFFFFFu);
  le16(cd, static_cast<uint32_t>(name.size()));
  le16(cd, 4 + 24);
  le16(cd, 0);
  le16(cd, 0);
  le16(cd, 0);
  le32(cd, 0);
  le32(cd, 0xFFFFFFFFu);
  put(cd, name);
  le16(cd, 1);
  le16(cd, 24);
  le64(cd, 5);
  le64(cd, 5);
  le64(cd, 0);
  z.insert(z.end(), cd.begin(), cd.end());
  const auto rec = static_cast<uint64_t>(z.size());
  le32(z, 0x06064b50);
  le64(z, 44);
  le16(z, 45);
  le16(z, 45);
  le32(z, 0);
  le32(z, 0);
  le64(z, 1);
  le64(z, 1);
  le64(z, cd.size());
  le64(z, cdAt);
  le32(z, 0x07064b50);
  le32(z, 0);
  le64(z, rec);
  le32(z, 1);
  le32(z, 0x06054b50);
  le16(z, 0xFFFF);
  le16(z, 0xFFFF);
  le16(z, 0xFFFF);
  le16(z, 0xFFFF);
  le32(z, 0xFFFFFFFFu);
  le32(z, 0xFFFFFFFFu);
  le16(z, 0);
  auto sh = std::make_shared<Shared>();
  TestSource src(z, sh, "z64.zip");
  std::vector<ZipEntry> dir;
  std::string err;
  expect(readZipDirectory(src, dir, &err) && dir.size() == 1 && dir[0].size == 5 && dir[0].packed == 5 && dir[0].offset == 0 && dir[0].name == name,
         "a ZIP64 archive reads (" + err + ")");

  // Names: UTF-8 without the flag stays; the old code page is converted; the flag is honoured.
  const std::string utf = "caf\xC3\xA9.wav";
  const Bytes n = makeZip({{utf, bytesOf("a"), false, 0}, {"caf\x82.wav", bytesOf("b"), false, 0}, {"b\\c.wav", bytesOf("c")}});
  TestSource s2(n, sh, "n.zip");
  expect(readZipDirectory(s2, dir, &err) && dir.size() == 3, "names read");
  expect(dir[0].name == utf, "UTF-8 names without the flag stay as they are");
  expect(dir[1].name == utf, "a name in the old code page becomes UTF-8");
  expect(dir[2].name == "b/c.wav", "backslashes become separators");

  const Bytes enc = makeZip({{"a.flp", bytesOf("x"), false, 0x0801}});
  TestSource s3(enc, sh, "e.zip");
  expect(readZipDirectory(s3, dir, &err) && dir[0].encrypted && !extractZipEntries(s3, dir, (fs::temp_directory_path() / "k42-enc").u8string(), nullptr, {}, &err) &&
             err.find("password") != std::string::npos,
         "a password protected archive says so");
  const Bytes evil = makeZip({{"good.wav", bytesOf("x")}, {"../escape.wav", bytesOf("y")}});
  TestSource s4(evil, sh, "v.zip");
  const fs::path evilDir = fs::temp_directory_path() / "k42-evil";
  fs::remove_all(evilDir);
  expect(readZipDirectory(s4, dir, &err) && !extractZipEntries(s4, dir, evilDir.u8string(), nullptr, {}, &err) && !fs::exists(evilDir / "good.wav"),
         "an escaping name refuses the archive before anything is written");
  const Bytes tiny = bytesOf("not an archive at all, and not long enough either");
  TestSource s5(tiny, sh, "t.zip");
  expect(!readZipDirectory(s5, dir, &err), "not a zip: refused");
}

// ---------------------------------------------------------------- which project

archive::ProjectPick pickOf(const std::vector<std::string>& names, const std::string& stem) {
  std::vector<ZipEntry> v;
  for (const auto& n : names) {
    ZipEntry e;
    e.name = n;
    e.size = n.back() == '/' ? 0 : 10;
    v.push_back(e);
  }
  return archive::pickProject(v, stem);
}

void testPick() {
  auto p = pickOf({"a.wav", "Song.flp", "b.wav"}, "Whatever");
  expect(p.names == std::vector<std::string>{"Song.flp"} && !p.ambiguous, "one project: no question");
  p = pickOf({"__MACOSX/._Song.flp", "__MACOSX/Song.flp", "._Song.flp", ".DS_Store", "Song.flp"}, "Song");
  expect(p.names == std::vector<std::string>{"Song.flp"} && !p.ambiguous, "macOS leftovers are not projects");
  p = pickOf({"First.flp", "Song.flp", "Other.flp"}, "Song");
  expect(p.names.size() == 3 && p.names[0] == "Song.flp" && !p.ambiguous, "the one named like the archive settles it");
  p = pickOf({"Beta.flp", "Alpha.flp"}, "Pack");
  expect(p.names.size() == 2 && p.names[0] == "Alpha.flp" && p.ambiguous, "two equal projects: a question, in a fixed order");
  p = pickOf({"Backup/Song (overwritten at 12h00).flp", "sub/Song 2.flp"}, "x");
  expect(p.names == std::vector<std::string>{"sub/Song 2.flp"} && !p.ambiguous, "copies in a backup folder do not count");
  p = pickOf({"Backup/One.flp"}, "x");
  expect(p.names == std::vector<std::string>{"Backup/One.flp"}, "unless nothing else is there");
  p = pickOf({"Song.flp", "a.gridline.json", "meta.json"}, "x");
  expect(p.names == std::vector<std::string>{"a.gridline.json"}, "a Gridline project comes first (as before)");
  p = pickOf({"Song.flp", "meta.json"}, "x");
  expect(p.names == std::vector<std::string>{"Song.flp"}, "a stray .json is not taken for the project");
  p = pickOf({"only.json"}, "x");
  expect(p.names == std::vector<std::string>{"only.json"}, "a lone .json is (any .json opens as a project)");
  p = pickOf({"Deep/er/A.flp", "Top.flp"}, "x");
  expect(p.names[0] == "Top.flp" && p.ambiguous, "the shallowest is the suggestion");
  expect(pickOf({"a.wav"}, "x").names.empty(), "no project");
}

// ---------------------------------------------------------------- unpacking for opening

void testUnpack(const fs::path& root) {
  const fs::path drive = root / "drive", cache = root / "cache";
  remotefs::setTestPrefixes({drive.u8string()});
  const fs::path zipPath = drive / "Music" / "My Song.zip";
  const Bytes song = bytesOf("flp-bytes");
  const Bytes z = makeZip({{"My Song.flp", song, true}, {"kick.wav", noise(70000)}, {"snare.wav", noise(90000, 5), true}, {"__MACOSX/._kick.wav", bytesOf("junk")}});
  expect(writeBytes(zipPath, z), "the archive is on the drive");
  const auto before = listing(drive);

  auto req = [&](std::shared_ptr<Shared> sh) {
    archive::UnpackRequest u;
    u.zipPath = zipPath.u8string();
    u.cacheRoot = cache.u8string();
    u.open = opener(z, std::move(sh));
    return u;
  };

  // First open: the local cache; nothing on the drive; progress adds up.
  {
    auto sh = std::make_shared<Shared>();
    auto u = req(sh);
    uint64_t seen = 0, total = 0;
    u.onBytes = [&](uint64_t n) { seen += n; };
    u.onTotal = [&](uint64_t n) { total = n; };
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(u, r, &err), "an archive on the drive unpacks (" + err + ")");
    expect(r.inCache && !r.reused && r.folder.rfind(cache.u8string(), 0) == 0, "into the local cache");
    expect(listing(drive) == before, "not one file appears on the drive");
    expect(fs::path(r.projectFile).filename() == "My Song.flp" && fs::is_regular_file(r.projectFile), "the project is found");
    expect(fs::is_regular_file(fs::path(r.folder) / "snare.wav") && fs::file_size(fs::path(r.folder) / "snare.wav") == 90000, "bundled samples are there");
    expect(!fs::exists(fs::path(r.folder) / "__MACOSX"), "macOS leftovers are skipped");
    expect(total > 0 && seen == total, "the bytes reported add up (" + std::to_string(seen) + " of " + std::to_string(total) + ")");
    expect(sh->bytes.load() <= z.size() + 70000, "the archive is read once (the end of the file twice at most)");
  }
  // Again: from the copy, not one byte of an entry read.
  {
    auto sh = std::make_shared<Shared>();
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(req(sh), r, &err) && r.reused && r.inCache && !r.offlineCopy, "an unchanged archive opens from its copy (" + err + ")");
    expect(sh->bytes.load() == 0, "without reading anything from the drive");
  }
  // A copy with a file gone is not whole: it is read again.
  {
    auto sh = std::make_shared<Shared>();
    archive::UnpackResult r0;
    std::string err;
    archive::unpackForOpening(req(sh), r0, &err);
    fs::remove(fs::path(r0.folder) / "kick.wav");
    archive::UnpackResult r;
    expect(archive::unpackForOpening(req(sh), r, &err) && !r.reused && fs::exists(fs::path(r.folder) / "kick.wav"), "a damaged copy is unpacked again");
  }
  // The drive is offline: the last copy, with a note.
  {
    auto u = req(std::make_shared<Shared>());
    u.open = [](const std::string& p, std::string* e) -> std::unique_ptr<remotefs::ByteSource> {
      if (e) *e = remotefs::readError(p, ENOTCONN);
      return nullptr;
    };
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(u, r, &err) && r.offlineCopy && r.reused && fs::is_regular_file(r.projectFile), "offline: the last copy opens (" + err + ")");
    u.zipPath = (drive / "Never Seen.zip").u8string();
    err.clear();
    expect(!archive::unpackForOpening(u, r, &err) && err.find("not answering") != std::string::npos, "offline, no copy: says why (" + err + ")");
  }
  // A failing read: the reason, and nothing half-unpacked.
  const fs::path zip2 = drive / "Other.zip";
  writeBytes(zip2, z);
  const auto cacheBefore = listing(cache);
  {
    auto sh = std::make_shared<Shared>();
    sh->failAt = static_cast<int64_t>(z.size() / 2);
    auto u = req(sh);
    u.zipPath = zip2.u8string();
    archive::UnpackResult r;
    std::string err;
    // (the directory at the end must read: the failure is in the entries)
    auto tailOk = std::make_shared<Shared>();
    u.open = [z, sh](const std::string& path, std::string*) -> std::unique_ptr<remotefs::ByteSource> {
      class Holed final : public remotefs::ByteSource {
       public:
        Holed(Bytes b, std::string p) : b_(std::move(b)), p_(std::move(p)) {}
        int64_t size() const override { return static_cast<int64_t>(b_.size()); }
        int64_t readAt(uint64_t at, void* out, size_t n, std::string* e) override {
          if (at > 75000 && at < b_.size() / 2 + 40000) {
            if (e) *e = remotefs::readError(p_, EIO);
            return -1;
          }
          const size_t m = std::min(n, b_.size() - static_cast<size_t>(at));
          std::memcpy(out, b_.data() + at, m);
          return static_cast<int64_t>(m);
        }

       private:
        Bytes b_;
        std::string p_;
      };
      return std::make_unique<Holed>(z, path);
    };
    expect(!archive::unpackForOpening(u, r, &err) && err.find("cannot read") != std::string::npos, "a read error is reported (" + err + ")");
    expect(listing(cache) == cacheBefore, "and leaves nothing in the cache");
  }
  // A cut archive (partial download): refused.
  {
    Bytes cut(z.begin(), z.begin() + static_cast<long>(z.size() / 2));
    auto u = req(std::make_shared<Shared>());
    u.zipPath = zip2.u8string();
    u.open = opener(cut, std::make_shared<Shared>());
    archive::UnpackResult r;
    std::string err;
    expect(!archive::unpackForOpening(u, r, &err) && !err.empty(), "half an archive is refused (" + err + ")");
    expect(listing(cache) == cacheBefore, "and leaves nothing");
  }
  // Damaged data: the CRC says so.
  {
    Bytes bad = z;
    bad[60000] ^= 0x40;
    auto u = req(std::make_shared<Shared>());
    u.zipPath = zip2.u8string();
    u.open = opener(bad, std::make_shared<Shared>());
    archive::UnpackResult r;
    std::string err;
    expect(!archive::unpackForOpening(u, r, &err) && (err.find("checksum") != std::string::npos || err.find("decompress") != std::string::npos), "damaged data is caught (" + err + ")");
    expect(listing(cache) == cacheBefore, "and leaves nothing");
  }
  // Cancel partway: nothing left.
  {
    auto sh = std::make_shared<Shared>();
    auto u = req(sh);
    u.zipPath = zip2.u8string();
    std::atomic<bool> stop{false};
    u.cancel = &stop;
    u.onBytes = [&](uint64_t) { stop = true; };
    archive::UnpackResult r;
    std::string err;
    expect(!archive::unpackForOpening(u, r, &err) && err == remotefs::kCancelled, "cancel stops it");
    expect(sh->bytes.load() < z.size() / 2, "at the next chunk");
    expect(listing(cache) == cacheBefore, "and leaves nothing");
  }
  // Several projects: the choice decides, and stopping there unpacks nothing.
  {
    const fs::path zip3 = drive / "Pack.zip";
    const Bytes zz = makeZip({{"Beta.flp", song}, {"Alpha.flp", song}});
    writeBytes(zip3, zz);
    auto u = req(std::make_shared<Shared>());
    u.zipPath = zip3.u8string();
    u.open = opener(zz, std::make_shared<Shared>());
    std::vector<std::string> asked;
    u.choose = [&](const std::vector<std::string>& n) {
      asked = n;
      return 1;
    };
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(u, r, &err) && fs::path(r.projectFile).filename() == "Beta.flp" && asked.size() == 2 && asked[0] == "Alpha.flp", "the chosen project opens");
    expect(r.otherProjects == 1, "the others are counted");
    u.zipPath = (drive / "Pack2.zip").u8string();
    writeBytes(u.zipPath, zz);
    const auto n0 = listing(cache);
    u.choose = [](const std::vector<std::string>&) { return -1; };
    archive::UnpackResult r2;
    expect(!archive::unpackForOpening(u, r2, &err) && err == remotefs::kCancelled && listing(cache) == n0, "stopping at the question unpacks nothing");
  }
  // An archive with no project.
  {
    const Bytes zz = makeZip({{"a.wav", bytesOf("x")}});
    auto u = req(std::make_shared<Shared>());
    u.zipPath = (drive / "None.zip").u8string();
    writeBytes(u.zipPath, zz);
    u.open = opener(zz, std::make_shared<Shared>());
    archive::UnpackResult r;
    std::string err;
    expect(!archive::unpackForOpening(u, r, &err) && err.find("no project") != std::string::npos, "an archive with no project says so");
  }
  // A local archive still unpacks beside itself (as before).
  {
    const fs::path local = root / "local" / "Beside.zip";
    const Bytes zz = makeZip({{"Beside.flp", song}, {"a.wav", bytesOf("x")}});
    writeBytes(local, zz);
    archive::UnpackRequest u;
    u.zipPath = local.u8string();
    u.cacheRoot = (root / "cache-local").u8string();
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(u, r, &err) && r.inCache && !r.fromDrive && fs::is_regular_file(fs::u8path(r.folder) / "a.wav") && !fs::exists(root / "local" / "Beside"),
           "a local archive unpacks into the cache, nothing beside it (" + err + ")");
    std::string proj;
    expect(unpackZippedProject(local.u8string(), (root / "local" / "again").u8string(), proj, &err) && fs::path(proj).filename() == "Beside.flp", "the plain unpacker too");
  }
  // Trimming: oldest first, a marked folder and the current one stay.
  {
    const fs::path c2 = root / "cache2";
    auto mk = [&](const char* n, int ageH, size_t bytes) {
      const fs::path d = c2 / n;
      writeBytes(d / "s.wav", noise(bytes));
      writeBytes(d / ".gridline-unpacked", bytesOf("gridline-unpacked 1\nx\n1\n1\ns.wav\n"));
      fs::last_write_time(d / ".gridline-unpacked", fs::file_time_type::clock::now() - std::chrono::hours(ageH));
    };
    mk("old", 100, 4000);
    mk("mid", 50, 4000);
    mk("new", 2, 4000);
    mk("fresh", 0, 4000);
    mk("kept", 200, 4000);
    Song s = toneSong((c2 / "kept" / "s.wav").u8string());
    expect(archive::pinUnpacked(s, c2.u8string()) == 1, "a saved project marks the folder it uses");
    const int gone = archive::trimCache(c2.u8string(), 4 * 4200);
    expect(gone >= 1 && !fs::exists(c2 / "old") && fs::exists(c2 / "kept") && fs::exists(c2 / "fresh"), "the oldest goes; a marked or just used copy stays");
    archive::trimCache(c2.u8string(), 0);
    expect(fs::exists(c2 / "kept") && fs::exists(c2 / "fresh") && !fs::exists(c2 / "mid") && !fs::exists(c2 / "new"), "down to the limit, never the marked one");
    fs::create_directories(c2 / "abandoned.part-1-1");
    fs::last_write_time(c2 / "abandoned.part-1-1", fs::file_time_type::clock::now() - std::chrono::hours(48));
    archive::sweepCache(c2.u8string(), ~uint64_t{0});
    expect(!fs::exists(c2 / "abandoned.part-1-1"), "a day old abandoned unpack goes");
  }
}

// ---------------------------------------------------------------- the loader

void testLoader(const fs::path& root) {
  const fs::path drive = root / "ldrive", cache = root / "lcache";
  remotefs::setTestPrefixes({drive.u8string()});
  const std::string wav = writeTone(root / "src" / "kick.wav");

  // A real project zip with a real sample, opened from the drive.
  const fs::path zipPath = drive / "Real.gridline.zip";
  std::error_code ec;
  fs::create_directories(drive, ec);
  const CollectReport cr = exportZippedProject(toneSong(wav), zipPath.u8string(), "Real");
  expect(cr.error.empty() && fs::exists(zipPath), "a project zip on the drive");
  const auto before = listing(drive);
  const uint64_t walks0 = remotefs::walksStarted();
  {
    Loader L;
    projload::Request r;
    r.path = zipPath.u8string();
    r.unpackCache = cache.u8string();
    r.sketch = false;
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready, "the zipped project opens from the drive (" + L.error() + ")");
    expect(L.result().fromZip && L.result().inCache, "from the local cache");
    expect(L.result().projectFile.rfind(cache.u8string(), 0) == 0, "the project is read from the cache");
    expect(L.samplesTotal() == 1 && L.samplesDone() == 1 && L.warnings() == 0, "its bundled sample is found");
    expect(L.result().missingRefs.empty(), "nothing is missing");
    expect(logHas(L, "nothing was written to the drive"), "the log says where it went");
    expect(listing(drive) == before, "nothing is written to the drive");
    L.clear();
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && logHas(L, "copy unpacked earlier"), "the second open uses the copy");
    L.clear();
  }
  expect(remotefs::walksStarted() == walks0, "opening walks no folder (counted)");
  sampleCache().waitIdle();

  // An FL Studio zipped loop package: the sample beside the project is found.
  {
    const fs::path fz = drive / "Fl.zip";
    flp::PackageOptions po;
    flp::PackageReport pr;
    std::string err;
    expect(flp::exportFlpZip(toneSong(wav), fz.u8string(), "Fl", po, pr, &err), "a package on the drive (" + err + ")");
    Loader L;
    projload::Request r;
    r.path = fz.u8string();
    r.unpackCache = cache.u8string();
    r.sketch = false;
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && L.result().fromFl && L.result().inCache, "the package opens as an FL Studio project (" + L.error() + ")");
    expect(L.samplesTotal() == 1, "its sample resolves into the cache");
    L.clear();
  }

  // A slow reader: bytes move while the interface steps.
  const Bytes big = makeZip({{"Slow.flp", bytesOf("x")}, {"a.wav", noise(4 * 1024 * 1024)}});
  {
    auto sh = std::make_shared<Shared>();
    sh->chunkDelayMs = 4;
    Loader L;
    projload::Request r;
    r.path = (drive / "Slow.zip").u8string();
    r.unpackCache = cache.u8string();
    r.open = opener(big, sh);
    r.sketch = false;
    L.start(r);
    double longest = 0, lastOverall = 0;
    uint64_t lastBytes = 0;
    int moved = 0;
    bool mono = true;
    while (L.active() && L.phase() == Phase::Reading) {
      const auto t = std::chrono::steady_clock::now();
      L.step(0.004);
      longest = std::max(longest, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
      if (L.bytesDone() > lastBytes) ++moved;
      mono = mono && L.overall() + 1e-9 >= lastOverall;
      lastBytes = L.bytesDone();
      lastOverall = L.overall();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(moved >= 3, "the bytes move on the bar (" + std::to_string(moved) + " steps)");
    expect(mono, "the bar never goes back");
    expect(longest < 0.1, "a step never waits for the read (" + std::to_string(longest) + " s)");
    L.cancel();
    L.clear();
  }

  // A read that stalls: the interface goes on, it says so, Cancel returns at once.
  {
    auto sh = std::make_shared<Shared>();
    sh->released = false;
    const std::set<std::string> cacheBefore = listing(cache);
    Loader L;
    projload::Request r;
    r.path = (drive / "Stall.zip").u8string();
    r.unpackCache = cache.u8string();
    r.open = opener(big, sh);
    r.stallSeconds = 0.2;
    L.start(r);
    double longest = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
      const auto t = std::chrono::steady_clock::now();
      L.step(0.004);
      longest = std::max(longest, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(sh->blocked.load() && L.phase() == Phase::Reading, "the read is stuck");
    expect(longest < 0.1, "the interface is not (" + std::to_string(longest) + " s)");
    expect(L.stalledFor() > 0.0, "a stalled drive is reported");
    const auto t = std::chrono::steady_clock::now();
    L.cancel();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    expect(L.phase() == Phase::Cancelled && took < 0.05, "Cancel returns at once (" + std::to_string(took) + " s)");
    expect(L.retiredRunning() == 1, "the stuck reader is set aside");
    L.clear();
    sh->release();
    for (int i = 0; i < 500 && L.retiredRunning() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(L.retiredRunning() == 0, "it stops at its next chunk when the read comes back");
    L.joinFinished();
    expect(listing(cache) == cacheBefore, "and takes what it started with it");
  }

  // Quit with a read stuck for good: the loader does not wait for it.
  {
    auto sh = std::make_shared<Shared>();
    sh->released = false;
    const auto t = std::chrono::steady_clock::now();
    {
      Loader L;
      projload::Request r;
      r.path = (drive / "Stuck.flp").u8string();
      r.open = opener(bytesOf("whatever"), sh);
      L.start(r);
      for (int i = 0; i < 200 && !sh->blocked.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      L.shutdown();
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    expect(took < 4.0, "quitting does not wait for a stuck read (" + std::to_string(took) + " s)");
    sh->release();  // (the left-behind thread ends on its own: it owns everything it touches)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // A failing read: the reason, in the window's error.
  {
    auto sh = std::make_shared<Shared>();
    sh->failAt = 0;
    Loader L;
    projload::Request r;
    r.path = (drive / "Fails.gridline.json").u8string();
    r.open = opener(bytesOf("{}"), sh);
    L.start(r);
    waitFor(L, Phase::Failed);
    expect(L.phase() == Phase::Failed && L.error().find("cannot read") != std::string::npos, "a read error fails with the reason (" + L.error() + ")");
    L.clear();
    r.open = nullptr;
    L.start(r);  // the file is not there at all
    waitFor(L, Phase::Failed);
    expect(L.phase() == Phase::Failed && L.error().find("connected and online") != std::string::npos, "a missing file on a drive says what to check (" + L.error() + ")");
    L.clear();
  }

  // Several projects: the load asks, Cancel stops at the question, finish() takes the suggestion.
  {
    Song s = toneSong(wav);
    const std::string text = projectToText(s);
    const Bytes zz = makeZip({{"B.gridline.json", bytesOf(text)}, {"A.gridline.json", bytesOf(text)}});
    Loader L;
    projload::Request r;
    r.path = (drive / "Two.zip").u8string();
    r.unpackCache = cache.u8string();
    r.open = opener(zz, std::make_shared<Shared>());
    r.askProject = true;
    r.sketch = false;
    L.start(r);
    for (int i = 0; i < 2000 && !L.awaitingChoice(); ++i) {
      L.step(0.001);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(L.awaitingChoice() && L.choices().size() == 2 && L.choices()[0] == "A.gridline.json", "several projects ask");
    expect(L.now().kind == projload::Now::Kind::Choosing, "the step says so");
    L.choose(1);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && fs::path(L.result().projectFile).filename() == "B.gridline.json", "the chosen one opens (" + L.error() + ")");
    L.clear();
    r.path = (drive / "Two b.zip").u8string();
    L.start(r);
    for (int i = 0; i < 2000 && !L.awaitingChoice(); ++i) {
      L.step(0.001);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    L.cancel();
    expect(L.phase() == Phase::Cancelled, "Cancel at the question");
    L.clear();
    r.path = (drive / "Two c.zip").u8string();
    L.start(r);
    L.finish();
    expect(L.phase() == Phase::Ready && fs::path(L.result().projectFile).filename() == "A.gridline.json", "without frames the suggestion opens");
    L.clear();
  }

  // Bare files: read through the same chunks; samples beside the project resolve on the drive.
  {
    const std::string onDrive = writeTone(drive / "bare" / "near.wav");
    const fs::path pf = drive / "bare" / "Bare.gridline.json";
    Song s = toneSong(onDrive);
    expect(saveProject(pf.u8string(), s), "a project on the drive");
    auto sh = std::make_shared<Shared>();
    Loader L;
    projload::Request r;
    r.path = pf.u8string();
    r.sketch = false;
    std::atomic<int> asked{0};
    r.collabProbe = [&](const std::string&) {
      ++asked;
      return false;
    };
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && L.samplesTotal() == 1 && !L.result().inCache, "a bare project opens with its sample (" + L.error() + ")");
    expect(L.bytesTotal() > 0 && L.bytesDone() == L.bytesTotal(), "the read counts");
    expect(asked.load() == 1, "whether it is collaborative is asked off the interface thread");
    L.clear();
    // A sample that is not reachable from here: clearly reported, the project still opens.
    Song s2 = toneSong("/Volumes/Another Machine/Samples/gone.wav");
    const fs::path pf2 = drive / "bare" / "Gone.gridline.json";
    saveProject(pf2.u8string(), s2);
    r.path = pf2.u8string();
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && L.warnings() == 1 && logHas(L, "Sample not found"), "a sample from another machine is reported, and the project opens");
    L.clear();
  }
  // A local file behaves as before, through the same reader.
  {
    const fs::path pf = root / "local2" / "Plain.gridline.json";
    fs::create_directories(pf.parent_path());
    saveProject(pf.u8string(), toneSong(wav));
    Loader L;
    projload::Request r;
    r.path = pf.u8string();
    r.sketch = false;
    r.collabProbe = [](const std::string&) {
      expect(false, "a local project is not asked on the worker");
      return false;
    };
    L.start(r);
    waitFor(L, Phase::Ready);
    expect(L.phase() == Phase::Ready && L.samplesTotal() == 1, "a local project opens as before");
    L.clear();
  }
}

// ---------------------------------------------------------------- never a crawl, never a write

void testNoCrawl(const fs::path& root) {
  const fs::path drive = root / "wdrive";
  remotefs::setTestPrefixes({drive.u8string()});
  writeTone(drive / "a" / "b" / "c" / "deep.wav");
  writeTone(drive / "top.wav");

  // The browser index skips a drive folder entirely (it would read every file to analyse it).
  {
    browse::Index idx;
    const uint64_t w0 = remotefs::walksStarted();
    idx.setRoots({{drive.u8string(), true}});
    idx.start();
    idx.waitIdle();
    expect(idx.size() == 0, "the browser index does not walk a drive");
    expect(remotefs::walksStarted() == w0, "(counted)");
    const fs::path local = root / "wlocal";
    writeTone(local / "x" / "here.wav");
    idx.setRoots({{local.u8string(), false}});
    idx.start();
    idx.waitIdle();
    expect(idx.size() == 1 && remotefs::walksStarted() == w0 + 1, "a local folder is still indexed");
    idx.stop();
  }
  // The relink search looks in the chosen folder only, on a drive.
  {
    const uint64_t w0 = remotefs::walksStarted();
    RelinkReport r = findReplacements({"C:/old/deep.wav", "C:/old/top.wav"}, drive.u8string());
    expect(r.shallow && r.found.count("C:/old/top.wav") == 1 && r.found.count("C:/old/deep.wav") == 0, "the relink search stays in the chosen folder of a drive");
    expect(r.indexed <= 1, "it did not go down");
    expect(remotefs::walksStarted() == w0 + 1, "one folder read");
    r = findReplacements({"C:/old/deep.wav"}, (root / "wlocal").u8string());
    expect(!r.shallow, "locally it searches sub-folders as before");
  }
  // Backups and the mount decisions.
  {
    BackupPolicy p;
    p.inProjectFolder = true;
    p.globalFolder = (root / "bk").u8string();
    BackupManager b(p);
    expect(b.folderFor((drive / "x.gridline.json").u8string()) == p.globalFolder, "backups of a project on a drive stay on this computer");
    expect(b.folderFor((root / "lp" / "x.gridline.json").u8string()) != p.globalFolder, "beside a local project as set");
  }
  expect(remotefs::isRemote((drive / "any" / "thing.flp").u8string()), "below a remote prefix");
  expect(!remotefs::isRemote((root / "wlocal" / "thing.flp").u8string()), "a local path is not remote");
  std::error_code ec;
  fs::create_directories(root / "links", ec);
  fs::create_symlink(drive, root / "links" / "Drive", ec);
  if (!ec) expect(remotefs::isRemote((root / "links" / "Drive" / "song.flp").u8string()), "a link to a drive counts");
  fs::create_symlink(root / "wlocal", root / "links" / "Local", ec);
  if (!ec) expect(!remotefs::isRemote((root / "links" / "Local" / "song.flp").u8string()), "a link to a local folder does not");
  expect(remotefs::remoteByName("/Users/me/Library/CloudStorage/GoogleDrive-a/My Drive/s.flp"), "macOS File Provider paths");
  expect(remotefs::remoteByName("/Users/me/Library/Mobile Documents/com~apple~CloudDocs/s.flp"), "iCloud Drive");
  expect(remotefs::remoteByName("/run/user/1000/gvfs/dav:host=x/s.flp"), "gvfs");
  expect(remotefs::remoteByName("\\\\server\\share\\s.flp"), "a network share");
  expect(!remotefs::remoteByName("/Users/me/Music/s.flp"), "a plain path");
  for (const char* t : {"macfuse", "osxfuse", "fuse.rclone", "fuse.gvfsd-fuse", "fuse.sshfs", "fuse.portal", "nfs4", "cifs", "smbfs", "webdav", "9p"})
    expect(remotefs::remoteFsType(t), std::string("remote: ") + t);
  for (const char* t : {"apfs", "hfs", "ext4", "btrfs", "xfs", "ntfs", "fuseblk", "exfat", "tmpfs"}) expect(!remotefs::remoteFsType(t), std::string("local: ") + t);

  // The off-thread stat and listing answer, without the caller waiting on them.
  {
    remotefs::Probe p = remotefs::probeWithin((drive / "top.wav").u8string(), 3000);
    expect(p.known && p.exists && !p.isDir, "a probe answers");
    p = remotefs::probeWithin((drive / "gone.wav").u8string(), 3000);
    expect(p.known && !p.exists, "for a missing file too");
    remotefs::DirState st = remotefs::listAsync(drive.u8string(), 60.0);
    for (int i = 0; i < 600 && !st.listing; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      st = remotefs::listAsync(drive.u8string(), 60.0);
    }
    expect(st.listing && st.listing->ok && st.listing->entries.size() == 2, "a folder lists in the background");
  }
  // Starting up with a project on a drive: the plan takes the drive's word for it later; the
  // recent list keeps a project a missing drive cannot confirm (the App's check, here its part).
  {
    recents::List l;
    l.touch("/nowhere/at/all/local.flp", 1);
    l.touch((drive / "offline" / "Song.zip").u8string(), 2);
    const size_t gone = l.pruneMissing([](const std::string& p) { return remotefs::isRemote(p) || recents::exists(p); });
    expect(gone == 1 && l.paths.size() == 1, "a project on an offline drive stays in the recent list, a deleted local one goes");
    const auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) (void)remotefs::isRemote((drive / "offline" / "Song.zip").u8string());
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    expect(took < 0.5, "asking is cheap enough for every frame (" + std::to_string(took) + " s per 1000)");
  }

  // The open path has no folder walk in it at all (the counter above only sees what declares itself).
  const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "src";
  for (const char* f : {"engine/ProjectLoad.cpp", "engine/Sample.cpp", "core/Zip.cpp", "core/Project.cpp", "core/flp/FlpReader.cpp", "core/flp/FlpProject.cpp", "core/flp/FlpChannels.cpp"}) {
    std::ifstream in(src / f);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) continue;  // (the tests can run away from the sources)
    expect(text.find("directory_iterator") == std::string::npos, std::string("no folder walk in ") + f);
  }
}

}  // namespace

int main() {
  const fs::path dir = gl::test::hermeticTestEnv("k42");
  testZipReadOnce();
  testInflate();
  testNamesAndZip64();
  testPick();
  testUnpack(dir);
  testLoader(dir);
  testNoCrawl(dir);
  remotefs::setTestPrefixes({});
  return finish();
}

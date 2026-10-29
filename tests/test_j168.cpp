// Gridline — J-168: FLP playlist records of every size (32 / 60 / 80 / 88
// bytes, a wider unknown layout, an unreadable one), built here from
// synthetic bytes (no FL Studio content): clips import, an unknown layout is
// read through its known leading fields or reported, and an FL 2026 record
// survives an export / import round trip.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "Check.h"

using namespace gl;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(static_cast<uint8_t>(v >> 8)); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {  // UTF-16LE + NUL
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

void p16(std::vector<uint8_t>& r, size_t at, uint32_t v) { r[at] = static_cast<uint8_t>(v), r[at + 1] = static_cast<uint8_t>(v >> 8); }
void p32(std::vector<uint8_t>& r, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) r[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}
void pf(std::vector<uint8_t>& r, size_t at, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  p32(r, at, u);
}

// One playlist record of `size` bytes: item 0x5001 = pattern 1, else channel `item`.
std::vector<uint8_t> rec(size_t size, uint32_t pos, uint16_t item, uint32_t len, int track, float so = -1, float eo = -1,
                         float fadeIn = 0, float gain = 1) {
  std::vector<uint8_t> r(size, 0);
  p32(r, 0, pos);
  p16(r, 4, 0x5000);
  p16(r, 6, item);
  p32(r, 8, len);
  p16(r, 12, static_cast<uint32_t>(499 - track));
  p16(r, 16, 0x78), p16(r, 18, 0x40);
  if (item > 0x5000) {
    p32(r, 24, static_cast<uint32_t>(so < 0 ? -1 : static_cast<int32_t>(so))), p32(r, 28, 0xFFFFFFFFu);
  } else {
    pf(r, 24, so), pf(r, 28, eo);
  }
  if (size >= 80) {
    pf(r, 36, fadeIn), pf(r, 52, gain);
    const double one = 1.0;
    std::memcpy(r.data() + 64, &one, 8);
    p32(r, 76, 0xFFFFFFFFu);
  }
  return r;
}

std::vector<uint8_t> cat(const std::vector<std::vector<uint8_t>>& v) {
  std::vector<uint8_t> o;
  for (const auto& r : v) o.insert(o.end(), r.begin(), r.end());
  return o;
}

// A project: one pattern (with a note), channel 0 an audio clip channel, one
// arrangement holding `items` as its playlist.
std::vector<uint8_t> project(const std::string& version, const std::vector<uint8_t>& items) {
  Builder B;
  B.text(199, version);
  B.d(156, 120000);
  B.w(65, 1);
  std::vector<uint8_t> n(24, 0);
  p16(n, 4, 0x4000), p16(n, 6, 1), p32(n, 8, 96), n[12] = 60, n[16] = 120, n[18] = 64, n[20] = 64, n[21] = 100, n[22] = 128, n[23] = 128;
  B.data(224, n);
  B.w(64, 0), B.b(21, 4), B.text(192, "Loop");  // audio clip channel
  B.w(64, 1), B.b(21, 0), B.text(192, "Hit");   // sampler
  B.w(99, 0);
  B.text(241, "Song");
  B.data(233, items);
  return B.file(96);
}

bool load(const std::vector<uint8_t>& bytes, Song& s, flp::Report& rep) {
  std::string err;
  return importFlpBytes(bytes, s, rep, &err, "/nonexistent");
}

bool warned(const flp::Report& rep, const std::string& what) {
  for (const auto& w : rep.warnings)
    if (w.find(what) != std::string::npos) return true;
  return false;
}

const Clip* clipAt(const Song& s, Tick start, ClipKind kind) {
  for (const auto& c : s.playlist)
    if (c.start == start && c.kind == kind) return &c;
  return nullptr;
}
}  // namespace

int main() {
  // ---- the stride detector on its own ----
  {
    auto stride = [](const std::vector<uint8_t>& b) {
      flp::Event e;
      e.id = 233, e.data = b.data(), e.size = static_cast<uint32_t>(b.size());
      return flp::playlistStride(e);
    };
    for (size_t k : {32u, 60u, 80u, 88u}) {
      const auto b = cat({rec(k, 0, 0x5001, 96, 0), rec(k, 96, 0x5001, 96, 1), rec(k, 192, 0, 96, 2)});
      expect(stride(b) == k, "stride: " + std::to_string(k) + "-byte records");
    }
    // 10 records of 88 bytes = 880 bytes = 11 x 80: the 0x5000 marks settle it.
    std::vector<std::vector<uint8_t>> ten;
    for (uint32_t i = 0; i < 10; ++i) ten.push_back(rec(88, i * 96, 0x5001, 96, 0));
    expect(stride(cat(ten)) == 88, "stride: 880 bytes of 88-byte records are not read as 80-byte ones");
    const auto wide = cat({rec(104, 0, 0x5001, 96, 0), rec(104, 96, 0x5001, 96, 0)});
    expect(stride(wide) == 104 && !flp::playlistStrideKnown(104), "stride: an unknown wider layout is found (104)");
    expect(stride(std::vector<uint8_t>(90, 0x11)) == 0, "stride: garbage has no layout");
    expect(stride({}) == 0, "stride: no records");
  }

  // ---- every known size imports its clips ----
  struct Ver {
    const char* version;
    size_t size;
  };
  for (const Ver& v : {Ver{"20.8.4.2576", 32}, Ver{"24.1.1.4285", 60}, Ver{"25.2.3.5164", 80}, Ver{"26.1.4.5356", 88}}) {
    const std::string tag = std::to_string(v.size) + "-byte records: ";
    const auto items = cat({rec(v.size, 0, 0x5001, 384, 0, 48), rec(v.size, 384, 0, 192, 3, 250.0f, 1250.0f, 20.0f, 0.5f),
                            rec(v.size, 768, 0x5001, 384, 9)});
    Song s;
    flp::Report rep;
    expect(load(project(v.version, items), s, rep), tag + "import succeeds");
    expect(s.playlist.size() == 3, tag + "all three clips");
    expect(rep.counts["pattern clips"] == 2 && rep.counts["audio clips"] == 1, tag + "report counts");
    expect(rep.warnings.empty() || !warned(rep, "playlist clips"), tag + "no layout warning");
    const Clip* p = clipAt(s, 0, ClipKind::Pattern);
    expect(p && p->track == 0 && p->length == 384 && p->offset == 48, tag + "pattern clip: track, length, offset");
    const Clip* p2 = clipAt(s, 768, ClipKind::Pattern);
    expect(p2 && p2->track == 9, tag + "second pattern clip on track 10");
    const Clip* a = clipAt(s, 384, ClipKind::Audio);
    expect(a && a->track == 3 && a->length == 192 && std::fabs(a->audioStart - 0.25) < 1e-6 && std::fabs(a->audioEnd - 1.25) < 1e-6,
           tag + "audio clip: track, length, sample window");
    if (v.size >= 80)
      expect(a && std::fabs(a->fadeIn - 0.02) < 1e-6 && std::fabs(a->gain - 0.5f) < 1e-6, tag + "audio clip: fade in and gain");
    expect(s.tracks.size() >= 10, tag + "tracks cover the clips");
    expect(s.lengthTicks() >= 768 + 384, tag + "song length covers the arrangement");
  }

  // ---- a wider layout nobody has seen yet: the leading fields, and a note ----
  {
    const auto items = cat({rec(112, 0, 0x5001, 384, 2), rec(112, 384, 0, 192, 4, 0.0f, 500.0f, 10.0f, 0.75f)});
    Song s;
    flp::Report rep;
    expect(load(project("27.0.0.6000", items), s, rep), "wider layout: import succeeds");
    expect(s.playlist.size() == 2, "wider layout: both clips");
    const Clip* a = clipAt(s, 384, ClipKind::Audio);
    expect(a && a->track == 4 && std::fabs(a->fadeIn - 0.01) < 1e-6 && std::fabs(a->gain - 0.75f) < 1e-6,
           "wider layout: the known leading fields (track, fades, gain)");
    expect(warned(rep, "Arrangement \"Song\"") && warned(rep, "112 bytes per clip") && warned(rep, "left out"),
           "wider layout: the report says what was left out");
  }

  // ---- an unreadable layout: nothing silently dropped ----
  {
    std::vector<uint8_t> junk(150, 0x11);
    Song s;
    flp::Report rep;
    expect(load(project("27.0.0.6000", junk), s, rep), "unreadable layout: import still succeeds");
    expect(s.playlist.empty(), "unreadable layout: no clips invented");
    expect(warned(rep, "Arrangement \"Song\"") && warned(rep, "150 bytes") && warned(rep, "without them"),
           "unreadable layout: the report names the arrangement and what was lost");
    expect(rep.counts["arrangements whose clips could not be read"] == 1, "unreadable layout: counted in the report");
    expect(s.patterns.size() == 1 && s.channels.size() == 2, "unreadable layout: the rest of the project imports");
  }

  // ---- FL 2026 records round trip through the export ----
  {
    const auto items = cat({rec(88, 0, 0x5001, 384, 0), rec(88, 384, 0, 192, 3, 250.0f, 1250.0f, 20.0f, 0.5f), rec(88, 96, 0x5001, 96, 1)});
    Song s;
    flp::Report rep;
    expect(load(project("26.1.4.5356", items), s, rep) && s.playlist.size() == 3, "round trip: FL 2026 project imports");
    for (bool origin : {true, false}) {
      const std::string tag = origin ? "round trip (origin): " : "round trip (encoders): ";
      flp::Bytes out;
      flp::ExportReport xr;
      flp::ExportOptions opt;
      opt.useOrigin = origin;
      opt.renderInstruments = false;
      expect(flp::exportFlp(s, out, xr, opt), tag + "exports");
      // The export says FL 25: its playlist records are FL 25's 80 bytes.
      flp::File f;
      std::string err;
      expect(flp::readBytes(out, f, &err), tag + "export parses");
      uint32_t stride = 0, bytes = 0;
      for (const auto& e : f.events)
        if (e.id == 233 && e.size) stride = flp::playlistStride(e), bytes += e.size;
      expect(stride == 80 && bytes == 3 * 80, tag + "written as 80-byte records");
      Song b;
      flp::Report rb;
      expect(load(out, b, rb) && b.playlist.size() == 3, tag + "re-import: all clips");
      const Clip* a = clipAt(b, 384, ClipKind::Audio);
      expect(a && a->track == 3 && a->length == 192 && std::fabs(a->audioStart - 0.25) < 1e-6 && std::fabs(a->fadeIn - 0.02) < 1e-6 &&
                 std::fabs(a->gain - 0.5f) < 1e-6,
             tag + "audio clip kept");
      const Clip* p = clipAt(b, 96, ClipKind::Pattern);
      expect(p && p->track == 1 && p->length == 96, tag + "pattern clip kept");
    }
  }
  return finish();
}

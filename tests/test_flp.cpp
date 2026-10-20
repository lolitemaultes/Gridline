// Gridline — FLP import: container reader, section splitter and the import
// spine, on small synthetic files built here (no FL Studio content).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/flp/Flp.h"
#include "Check.h"

using namespace gl;

namespace {
struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
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
std::vector<uint8_t> note(uint32_t pos, uint16_t ch, uint32_t len, uint16_t key, uint8_t vel) {
  std::vector<uint8_t> n(24, 0);
  auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) n[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); };
  put32(0, pos);
  n[4] = 0x00, n[5] = 0x40;  // flags
  n[6] = static_cast<uint8_t>(ch), n[7] = static_cast<uint8_t>(ch >> 8);
  put32(8, len);
  n[12] = static_cast<uint8_t>(key);
  n[16] = 120, n[18] = 64, n[20] = 64, n[21] = vel, n[22] = 128, n[23] = 128;
  return n;
}
}  // namespace

int main() {
  Builder B;
  B.text(199, "25.2.3.5164");
  B.d(156, 128000);  // 128 BPM
  B.b(17, 3), B.b(18, 4);
  B.text(194, "Synthetic");
  B.w(65, 1);  // pattern 1
  std::vector<uint8_t> notes;
  for (auto n : {note(0, 0, 96, 60, 100), note(96, 1, 48, 64, 90), note(192, 0, 96, 67, 110)}) notes.insert(notes.end(), n.begin(), n.end());
  B.data(224, notes);
  B.w(64, 0), B.b(21, 0), B.text(192, "Kick"), B.b(22, 1);  // sampler channel → insert 1
  B.w(64, 1), B.b(21, 2), B.text(192, "Lead");                // native plugin channel
  B.w(99, 0);                                                  // arrangement 0
  std::vector<uint8_t> item(60, 0);
  item[4] = 0x00, item[5] = 0x50;                       // pattern base 20480
  item[6] = 0x01, item[7] = 0x50;                       // item index base+1 = pattern 1
  item[8] = 0x80, item[9] = 0x01;                       // length 384
  item[12] = 0xF3, item[13] = 0x01;                     // track_rv 499 → track 0
  B.data(233, item);
  B.text(204, "Drums"), B.data(236, std::vector<uint8_t>(12, 0));
  for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
  B.d(147, 0);  // master
  B.text(204, "Kick bus"), B.data(236, std::vector<uint8_t>(12, 0));
  for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
  B.d(147, 0);  // insert 1

  flp::File f;
  std::string err;
  expect(flp::readBytes(B.file(96), f, &err), "synthetic FLP parses");
  expect(f.ppq == 96 && f.version == "25.2.3.5164", "header: PPQ and version");
  flp::Sections sec = flp::split(f);
  expect(sec.patterns.count(1) && sec.channels.size() == 2 && sec.arrangements.count(0), "split: pattern, channels, arrangement");
  expect(sec.inserts.size() == 2, "split: two mixer inserts");

  // Event 172 size quirk (FL 25.2.3+): selector 1 → 2-byte value.
  Builder Q;
  Q.ev = {172, 1, 1, 0, 192, 4, 'a', 0, 'b', 0};
  flp::File fq;
  expect(flp::readBytes(Q.file(96), fq, &err) && fq.events.size() == 2 && fq.events[1].id == 192, "event 172 quirk keeps the stream in sync");

  // Full import from bytes via a temporary file.
  std::string path = "build_flp_test.flp";
  if (FILE* fp = std::fopen(path.c_str(), "wb")) {
    auto bytes = B.file(96);
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
  }
  Song s;
  flp::Report rep;
  bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  std::remove(path.c_str());
  expect(ok, "import succeeds");
  expect(std::fabs(s.tempo - 128.0) < 1e-9 && s.timeSigNum == 3 && s.ppq == 96 && s.info.title == "Synthetic", "project info");
  expect(s.channels.size() == 2 && s.channels[0].name == "Kick" && s.channels[0].plugin.type == "gl.sampler", "channels");
  expect(s.mixer.size() == 2 && s.mixer[1].name == "Kick bus" && s.channels[0].mixerTrack == s.mixer[1].id, "mixer names + routing");
  size_t total = 0;
  for (const auto& [ch, ns] : s.patterns.at(0).notes) total += ns.size();
  expect(s.patterns.size() == 1 && total == 3, "notes import");
  expect(s.playlist.size() == 1 && s.playlist[0].track == 0 && s.playlist[0].length == 384, "pattern clip on track 1");
  expect(s.autoCrossfades, "J-43: no event 44 -> automatic crossfades on");

  // ---- J-43: audio clip fade records and the automatic crossfades switch ----
  auto rec = [](uint32_t start, uint32_t len, int track, float in, float tin, float out, float tout, uint32_t flags) {
    std::vector<uint8_t> r(80, 0);
    auto p32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) r[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); };
    auto pf = [&](size_t at, float v) { uint32_t u; std::memcpy(&u, &v, 4); p32(at, u); };
    p32(0, start);
    r[4] = 0x00, r[5] = 0x50;  // base; item 0 = channel 0
    p32(8, len);
    r[12] = static_cast<uint8_t>((499 - track) & 0xFF), r[13] = static_cast<uint8_t>((499 - track) >> 8);
    pf(36, in), pf(40, tin), pf(44, out), pf(48, tout), pf(52, 1.0f);
    p32(56, flags);
    const double one = 1.0;
    std::memcpy(r.data() + 64, &one, 8);
    return r;
  };
  auto audioSong = [&](int e44, const std::vector<std::vector<uint8_t>>& recs, Song& out) {
    Builder A;
    A.text(199, "25.2.3.5164");
    A.d(156, 120000);  // 120 BPM, 96 PPQ: 192 ticks = 1 s
    if (e44 >= 0) A.b(44, static_cast<uint8_t>(e44));
    A.w(64, 0), A.b(21, 4), A.text(192, "Vox");  // audio clip channel
    A.w(99, 0);
    for (const auto& r : recs) A.data(233, r);
    const std::string p = "build_flp_test_j43.flp";
    if (FILE* fp = std::fopen(p.c_str(), "wb")) {
      auto bytes = A.file(96);
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
    }
    flp::Report r;
    const bool good = importFlp(p, out, r, &err, "/nonexistent");
    std::remove(p.c_str());
    return good;
  };
  {  // hand-set fades: lengths, tensions, curves, declicker modes; switch off
    Song a;
    const bool good = audioSong(0, {rec(0, 384, 0, 150.0f, -3.9586f, 250.0f, 6.72f, 1u | 2u | (2u << 2) | (1u << 8) | (1u << 14))}, a);
    const Clip* c = good && a.playlist.size() == 1 ? &a.playlist[0] : nullptr;
    expect(good && !a.autoCrossfades, "J-43: event 44 = 0 -> automatic crossfades off");
    expect(c && c->kind == ClipKind::Audio && std::fabs(c->fadeIn - 0.15) < 1e-6 && std::fabs(c->fadeOut - 0.25) < 1e-6,
           "J-43: fade lengths from @36 / @44 (ms)");
    expect(c && c->fadeInTension == -3.9586f && c->fadeOutTension == 6.72f && c->pitch == 0.0f,
           "J-43: tensions from @40 / @48 (@48 is not a clip pitch)");
    expect(c && c->fadeInManual && c->fadeOutManual && c->fadeInShape == 1 && c->fadeOutShape == 0 && c->fadeInDeclick == 2 &&
               c->fadeOutDeclick == 1,
           "J-43: flags @56: manual bits, curve (bits 8-9 / 12-13), declicker (bits 2-4 / 14-16)");
  }
  {  // stored automatic crossfades: the one equal to the overlap is derived again, a different one is kept
    Song a;
    const bool good = audioSong(1, {rec(0, 384, 0, 0, 0, 500.0f, 0, 0), rec(288, 384, 0, 500.0f, 0, 0, 0, 0),
                                    rec(0, 384, 1, 0, 0, 123.0f, 0, 0), rec(288, 384, 1, 123.0f, 0, 0, 0, 0)}, a);
    expect(good && a.autoCrossfades && a.playlist.size() == 4, "J-43: event 44 = 1 -> automatic crossfades on");
    if (good && a.playlist.size() == 4) {
      const Clip &x = a.playlist[0], &y = a.playlist[1], &u = a.playlist[2], &v = a.playlist[3];
      expect(x.fadeOut == 0 && y.fadeIn == 0 && !x.fadeOutManual && !y.fadeInManual,
             "J-43: a stored crossfade equal to the overlap becomes automatic again (follows the clips)");
      expect(std::fabs(u.fadeOut - 0.123) < 1e-6 && std::fabs(v.fadeIn - 0.123) < 1e-6 && !u.fadeOutManual,
             "J-43: a stored crossfade Gridline would not derive is kept, so playback matches FL");
    }
  }
  return finish();
}

// Gridline — playlists saved before FL 12.9 (GitHub issue #51): the track a clip
// is stored on counts up from the last track, and the playlist had 99 tracks up
// to FL 10, 199 from FL 11 to 12.8 and 500 since. Synthetic FLP data only: clips
// and track settings land on the same tracks for each era, and a file without a
// version is read from its clips.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/flp/Flp.h"
#include "Check.h"

using namespace gl;

namespace {

struct Builder {
  std::vector<uint8_t> ev;
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
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  void mixer(int inserts) {
    for (int ins = 0; ins < inserts; ++ins) {
      data(236, std::vector<uint8_t>(12, 0));
      for (int k = 0; k < 10; ++k) w(98, static_cast<uint16_t>(k));
      d(147, 0);
    }
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

void u16(std::vector<uint8_t>& v, size_t at, uint32_t x) { v[at] = static_cast<uint8_t>(x), v[at + 1] = static_cast<uint8_t>(x >> 8); }
void u32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}
void f32(std::vector<uint8_t>& v, size_t at, float x) {
  uint32_t b;
  std::memcpy(&b, &x, 4);
  u32(v, at, b);
}

// A 32-byte playlist record placing pattern 1 with the raw stored track value.
std::vector<uint8_t> clip(uint32_t pos, uint16_t stored) {
  std::vector<uint8_t> r(32, 0);
  u32(r, 0, pos), u16(r, 4, 0x5000), u16(r, 6, 0x5001), u32(r, 8, 96), u16(r, 12, stored), u16(r, 18, 0x40);
  f32(r, 24, -1.0f), f32(r, 28, -1.0f);
  return r;
}
// A track record (238): 1-based index and height.
std::vector<uint8_t> track(uint32_t index, float height) {
  std::vector<uint8_t> t(66, 0);
  u32(t, 0, index), t[12] = 1, f32(t, 13, height), u32(t, 30, 5), u32(t, 38, 1);
  return t;
}

// A pre-12.9 file (no Arrangement.New): one channel, pattern 1, the given clips,
// and track 2 at height 0.975.
bool importLegacy(const std::string& version, const std::vector<uint16_t>& stored, Song& s, const char* name) {
  Builder B;
  if (!version.empty()) B.text(199, version);
  B.d(156, 120000);
  B.w(65, 1);
  B.w(64, 0), B.text(203, "Lead");
  std::vector<uint8_t> items;
  uint32_t pos = 0;
  for (uint16_t v : stored) {
    auto r = clip(pos, v);
    items.insert(items.end(), r.begin(), r.end());
    pos += 96;
  }
  B.data(233, items);
  B.data(238, track(2, 0.975f));
  B.mixer(2);
  std::string path = (std::filesystem::temp_directory_path() / name).string();
  if (FILE* fp = std::fopen(path.c_str(), "wb")) {
    auto bytes = B.file(96);
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
  }
  flp::Report rep;
  std::string err;
  bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  std::remove(path.c_str());
  return ok;
}

bool tracksAre(const Song& s, const std::vector<int>& want) {
  if (s.playlist.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i)
    if (s.playlist[i].track != want[i]) return false;
  return true;
}

bool heightOnTrack2(const Song& s) { return s.tracks.size() > 1 && std::fabs(s.tracks[1].height - 0.975f) < 1e-4f; }
}  // namespace

int main() {
  // The base on its own.
  expect(flp::playlistTrackBase("10.0.0", 98) == 98, "base: FL 10 has 99 tracks");
  expect(flp::playlistTrackBase("9.1.0", 50) == 98, "base: FL 9 has 99 tracks");
  expect(flp::playlistTrackBase("11.0.3", 98) == 198, "base: FL 11 has 199 tracks");
  expect(flp::playlistTrackBase("12.8.1", 120) == 198, "base: FL 12.8 has 199 tracks");
  expect(flp::playlistTrackBase("12.9.0", 10) == 499, "base: FL 12.9 has 500 tracks");
  expect(flp::playlistTrackBase("20.8.4", 499) == 499, "base: FL 20 has 500 tracks");
  expect(flp::playlistTrackBase("10.0.0", 150) == 198, "base: a stored value past 98 widens an FL 10 file");
  expect(flp::playlistTrackBase("11.0.3", 300) == 499, "base: a stored value past 198 widens an FL 11 file");
  expect(flp::playlistTrackBase("", 98) == 98, "base: no version, values up to 98");
  expect(flp::playlistTrackBase("", 99) == 198, "base: no version, values up to 198");
  expect(flp::playlistTrackBase("", 400) == 499, "base: no version, values past 198");
  expect(flp::playlistTrackBase("", -1) == 98, "base: no version, no clips");
  expect(flp::playlistTrackBase("garbage", 98) == 98, "base: unreadable version reads the clips");

  // FL 10 (the issue's case): stored 90..98 are tracks 9..1.
  {
    Song s;
    expect(importLegacy("10.0.0", {98, 90, 92, 97}, s, "gl_k209_fl10.flp"), "FL 10: import succeeds");
    expect(tracksAre(s, {0, 8, 6, 1}), "FL 10: clips on tracks 1 to 9, not 101 to 109");
    expect(heightOnTrack2(s), "FL 10: the track 2 height stays on track 2");
    expect(s.tracks.size() < 20, "FL 10: the playlist is not padded to 100 tracks");
  }
  // FL 11 to 12.8: 199 tracks, stored 198 is track 1.
  {
    Song s;
    expect(importLegacy("11.0.3", {198, 190, 197}, s, "gl_k209_fl11.flp"), "FL 11: import succeeds");
    expect(tracksAre(s, {0, 8, 1}), "FL 11: clips count down from 198");
    expect(heightOnTrack2(s), "FL 11: the track 2 height stays on track 2");
  }
  {
    Song s;
    expect(importLegacy("11.1.0", {98}, s, "gl_k209_fl11_low.flp"), "FL 11 low tracks: import succeeds");
    expect(tracksAre(s, {100}), "FL 11: a clip on track 101 stays there");
  }
  // A 12.9-style value in a file without arrangements: 500 tracks.
  {
    Song s;
    expect(importLegacy("12.9.1", {499, 491}, s, "gl_k209_fl129.flp"), "FL 12.9: import succeeds");
    expect(tracksAre(s, {0, 8}), "FL 12.9: clips count down from 499");
    expect(heightOnTrack2(s), "FL 12.9: the track 2 height stays on track 2");
  }
  // No version: the clips decide.
  {
    Song s;
    expect(importLegacy("", {98, 94}, s, "gl_k209_nover99.flp"), "no version, 99 tracks: import succeeds");
    expect(tracksAre(s, {0, 4}), "no version: values up to 98 read as 99 tracks");
    expect(heightOnTrack2(s), "no version: the track 2 height stays on track 2");
  }
  {
    Song s;
    expect(importLegacy("", {198, 150}, s, "gl_k209_nover199.flp"), "no version, 199 tracks: import succeeds");
    expect(tracksAre(s, {0, 48}), "no version: values up to 198 read as 199 tracks");
  }
  {
    Song s;
    expect(importLegacy("", {499, 300}, s, "gl_k209_nover500.flp"), "no version, 500 tracks: import succeeds");
    expect(tracksAre(s, {0, 199}), "no version: values past 198 read as 500 tracks");
  }
  return finish();
}

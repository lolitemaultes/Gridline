// Gridline - tracker modules on a local collection of real modules (not in the repository).
// Set GRIDLINE_TRACKER_CORPUS to a folder of .xm / .mod files; without it the test is skipped.
// Every module must read and convert, the song must hold what the module holds, and truncated
// copies must read or be refused cleanly.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../../src/core/TrackerData.h"
#include "../../src/core/TrackerImport.h"
#include "../../src/core/TrackerStream.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace fs = std::filesystem;

namespace {

void check(bool ok, const std::string& name) { expect(ok, name.c_str()); }

void testModule(const fs::path& p) {
  const std::string name = p.filename().u8string();
  std::ifstream in(p, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  trk::Module m;
  std::string err;
  if (!trk::parseModule(bytes, m, &err)) {
    check(false, name + ": reads (" + err + ")");
    return;
  }
  int withAudio = 0, sampleFiles = 0;
  for (size_t i = 1; i < m.instruments.size(); ++i) {
    withAudio += m.instruments[i].hasAudio() ? 1 : 0;
    for (const auto& s : m.instruments[i].samples) sampleFiles += s.data.empty() ? 0 : 1;
  }
  Song song;
  trk::ImportReport rep;
  trk::ImportOptions o;
  if (!trk::buildSong(m, name, o, song, rep, &err)) {
    check(false, name + ": converts (" + err + ")");
    return;
  }
  check(rep.instruments >= withAudio && static_cast<int>(song.channels.size()) == rep.instruments, name + ": a channel for every instrument with samples");
  check(rep.samples == sampleFiles, name + ": every sample carried over");
  check(rep.rows > 0 && rep.clips > 0 && !song.patterns.empty() && rep.notes > 0, name + ": rows, clips, patterns and notes");
  // Every note is a valid stream on one of the module's channels, and inside its pattern.
  bool streams = true, inside = true;
  for (const auto& pat : song.patterns) {
    const Tick len = static_cast<Tick>(pat.bars) * pat.stepsPerBar * (song.ppq / std::max(1, pat.stepsPerBar / 4));
    for (const auto& [cid, notes] : pat.notes)
      for (const auto& n : notes) {
        trk::StreamHeader h;
        trk::StreamReader r;
        streams = streams && r.open(n.tracker.data(), n.tracker.size(), h) && h.lane < m.channels && n.tracker.size() <= trk::kStreamMax;
        inside = inside && n.start >= 0 && n.length > 0 && n.start + n.length <= len;
      }
  }
  check(streams, name + ": every note carries a valid stream of one of its channels");
  check(inside, name + ": every note lies inside its pattern");
  // Clips follow each other without gaps on the song track.
  Tick at = 0;
  bool contiguous = true;
  for (const auto& c : song.playlist)
    if (c.kind == ClipKind::Pattern) {
      contiguous = contiguous && c.start == at && c.length > 0;
      at += c.length;
    }
  check(contiguous, name + ": the order list plays clip after clip");
  // Instrument texts read back with their samples.
  bool texts = true;
  for (const auto& c : song.channels) {
    trk::InstrumentText t;
    texts = texts && trk::decodeInstrument(c.plugin.data, t);
  }
  check(texts, name + ": instruments read back");
  // Truncated copies read or are refused; neither crashes.
  int read = 0;
  for (int k = 1; k < 24; ++k) {
    const size_t n = bytes.size() * static_cast<size_t>(k) / 24;
    trk::Module tm;
    std::string e;
    if (!trk::parseModule(bytes.data(), n, tm, &e)) continue;
    ++read;
    Song ts;
    trk::ImportReport tr;
    trk::ImportOptions to;
    to.maxSeconds = 120;
    trk::buildSong(tm, name, to, ts, tr, &e);
  }
  std::printf("%s: %d channels, %d instruments, %d samples, %d notes, %d patterns (%d variants), %d clips, %.1f s, %d of 23 cut copies read\n", name.c_str(),
              m.channels, rep.instruments, rep.samples, rep.notes, rep.patterns, rep.variants, rep.clips, rep.seconds, read);
}

}  // namespace

int main() {
  const char* dir = std::getenv("GRIDLINE_TRACKER_CORPUS");
  std::error_code ec;
  if (!dir || !fs::is_directory(fs::u8path(dir), ec)) {
    std::printf("tracker corpus: GRIDLINE_TRACKER_CORPUS not set, skipped\n");
    return finish();
  }
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec))
    if (e.is_regular_file() && trk::isModulePath(e.path().u8string())) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto& f : files) testModule(f);
  check(!files.empty(), "tracker corpus: modules found");
  return finish();
}

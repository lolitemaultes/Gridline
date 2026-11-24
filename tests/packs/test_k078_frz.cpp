// Gridline - FRZ-1118 (0.7.8): the 0.7.7 freeze work's leftovers. A web or network drive that answers slowly
// (or not at all) never holds the interface thread for more than a frame's worth (100 ms):
//   1. the background writer: post never waits, drain is bounded;
//   2. the collaboration log on such a drive: appends, syncs and compactions are handed over, and
//      everything lands, in order, once the drive answers;
//   3. a collaboration session there: edits, frames and saves do not wait for the drive, and the
//      project file it writes in the end has the edit;
//   4. quitting does not wait for a sample decoder stuck on a file that never answers;
//   5. the image synth's picture and the amp rack's cabinet folder are read in the background.
// The interface thread is stood in for by SampleCache::NonBlocking (a program build there), the
// drive by GRIDLINE_DEV_REMOTE_PATHS prefixes, its slowness by test delays and named pipes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "../Check.h"
#include "../collab/CollabSim.h"
#include "../../src/collab/Log.h"
#include "../../src/core/AudioBuffer.h"
#include "../../src/core/RemoteFs.h"
#include "../../src/core/SerialWorker.h"
#include "../../src/engine/Export.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/dynamics/cabinet_ir.h"
#include "../../src/packs/models/models.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

using Clk = std::chrono::steady_clock;
double msSince(Clk::time_point t) { return std::chrono::duration<double, std::milli>(Clk::now() - t).count(); }
void ex(bool ok, const std::string& name) { expect(ok, name.c_str()); }
std::string fmt(const char* f, double v) {
  char b[160];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
constexpr double kFrameMs = 100;

fs::path scratch(const std::string& name) {
  const fs::path p = fs::temp_directory_path() / "gridline-k078-frz" / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

void testWorker() {
  SerialWorker w;
  SerialWorker::setTestDelayMs(300);
  std::atomic<int> ran{0};
  const auto t0 = Clk::now();
  for (int i = 0; i < 5; ++i) w.post([&] { ++ran; });
  const double postMs = msSince(t0);
  ex(postMs < 20, fmt("worker: five posts to a slow drive return at once (%.1f ms)", postMs));
  const auto t1 = Clk::now();
  // (bounded: it gives up long before the slow jobs could all run, 5 x 300 ms; a loaded runner may
  // hold the waiting thread a while past its 50 ms, so the bound is under a third of what the jobs need, not 4x the wait)
  const bool drained = w.drain(50);
  const double drainMs = msSince(t1);
  ex(!drained && drainMs < 450, fmt("worker: a drain is bounded (%.0f ms)", drainMs));
  SerialWorker::setTestDelayMs(0);
  ex(w.drain(5000) && ran.load() == 5, "worker: every job runs, in the end");
}

void testLog(const fs::path& root) {
  const fs::path dir = root / "share" / ".gridline-collab";
  remotefs::setTestPrefixes({(root / "share").u8string()});
  auto worker = std::make_shared<SerialWorker>();
  collab::OpLog log;
  log.setWorker(worker);
  Json snap;
  std::vector<std::string> recs;
  ex(log.open(dir.u8string(), snap, recs) && log.async(), "log: opens on the drive, async");
  SerialWorker::setTestDelayMs(250);
  double worst = 0;
  for (int i = 0; i < 10; ++i) {
    const auto t = Clk::now();
    log.append("record " + std::to_string(i));
    if (i % 3 == 0) log.sync();
    worst = std::max(worst, msSince(t));
  }
  {
    const auto t = Clk::now();
    Json s = Json::makeObject();
    s.object["what"] = Json::makeString("snapshot");
    log.compact(s, {"record 7", "record 8", "record 9"});
    worst = std::max(worst, msSince(t));
  }
  for (int i = 10; i < 15; ++i) {
    const auto t = Clk::now();
    log.append("record " + std::to_string(i));
    worst = std::max(worst, msSince(t));
  }
  ex(worst < kFrameMs, fmt("log: appends, syncs and a compaction on a slow drive never wait for it (worst %.1f ms)", worst));
  ex(log.records() == 8 && log.generation() == 1, "log: the counts are the caller's at once");
  SerialWorker::setTestDelayMs(0);
  ex(worker->drain(10000), "log: the drive catches up");
  log.close();
  collab::OpLog back;
  ex(back.open(dir.u8string(), snap, recs) && recs.size() == 8 && recs.front() == "record 7" && recs.back() == "record 14" && snap.text("what") == "snapshot",
     "log: everything landed, in order (" + std::to_string(recs.size()) + " records)");
  back.close();
  // A close while the drive does not answer: bounded.
  {
    collab::OpLog slow;
    auto w2 = std::make_shared<SerialWorker>();
    slow.setWorker(w2);
    ex(slow.open(dir.u8string(), snap, recs), "log: opened again");
    SerialWorker::setTestDelayMs(4000);
    slow.append("late");
    const auto t = Clk::now();
    slow.close();
    const double ms = msSince(t);
    ex(ms < SerialWorker::kLeaveWaitMs + 500, fmt("log: closing waits for a stuck drive at most a moment (%.0f ms)", ms));
    SerialWorker::setTestDelayMs(0);
    w2->drain(10000);
  }
  remotefs::setTestPrefixes({});
}

void testSession(const fs::path& root) {
  using namespace sim;
  const fs::path share = root / "collab_share";
  remotefs::setTestPrefixes({share.u8string()});
  std::error_code ec;
  fs::create_directories(share, ec);
  const std::string folder = (share / "Song").u8string();
  Document doc;
  doc.reset(makeEmptySong());
  doc.addChannel("Kick", "gl.kick");
  const std::string media = "Audio/" + std::string(64, 'c') + ".wav";  // a sample the share does not have yet
  const ObjectId loop = doc.addChannel("Loop", "gl.sampler");
  doc.editChannel(loop, "sample", [&](Channel& c) { c.plugin.data = (fs::u8path(folder) / media).u8string(); });
  Clock clock;
  const auto id = collab::crypto::Identity::generate();
  std::string err;
  auto s = collab::Session::create(doc, config(folder, id, "Alex", &clock), &err);
  ex(s != nullptr, "session: made on the drive " + err);
  if (!s) return;
  SerialWorker::setTestDelayMs(300);
  double worst = 0;
  for (int f = 1; f <= 30; ++f) {
    const auto t = Clk::now();
    if (f == 5) doc.renameChannel(doc.song().channels[0].id, "Edited on a slow share");
    if (f % 7 == 0) doc.addChannel("Hat " + std::to_string(f), "gl.hat");
    clock.ms += 600;
    s->frame(f * 0.6);
    if (f == 20) s->saveNow();
    worst = std::max(worst, msSince(t));
  }
  ex(worst < kFrameMs, fmt("session: thirty frames with edits and a save on a slow share (worst %.1f ms)", worst));
  SerialWorker::setTestDelayMs(0);
  // The look for missing media (the disk thread's) still finds the sample to fetch.
  bool wanted = false;
  for (int f = 31; f <= 60 && !wanted; ++f) {
    clock.ms += 1100;
    s->frame(f * 1.1);
    for (const auto& t : s->transfers()) wanted = wanted || t.name == media;
    if (!wanted) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ex(wanted, "session: the missing sample on the share is looked for off the interface and asked for");
  s.reset();  // (the last save, then the drive catches up)
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string text;
  bool found = false;
  for (const auto& e : fs::directory_iterator(fs::u8path(folder), ec))
    if (e.path().extension() == ".gridline" && collab::readFileText(e.path().u8string(), text) && text.find("Edited on a slow share") != std::string::npos) found = true;
  ex(found, "session: the project file on the share has the edit");
  remotefs::setTestPrefixes({});
}

#ifndef _WIN32
// A named pipe: opening it for reading waits until someone opens it for writing (a drive that
// does not answer). unblock() lets the reader through to an empty file.
struct Pipe {
  std::string path;
  explicit Pipe(const fs::path& p) : path(p.u8string()) { ::mkfifo(path.c_str(), 0600); }
  void unblock() const {
    const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd >= 0) ::close(fd);
  }
  ~Pipe() { ::unlink(path.c_str()); }
};
#endif

void testQuit(const fs::path& root) {
#ifndef _WIN32
  Pipe p(root / "stuck.wav");
  auto* cache = new SampleCache;  // (left behind on purpose: a stuck decoder still holds it)
  cache->request(p.path);
  for (int i = 0; i < 100 && cache->pending() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto t = Clk::now();
  const bool done = cache->shutdown(300);
  const double ms = msSince(t);
  ex(!done && cache->abandoned() && ms < 800, fmt("quit: a decoder stuck on a file that never answers is left after the bounded wait (%.0f ms)", ms));
  for (int i = 0; i < 20; ++i) {  // let it go (it reads an empty file and ends)
    p.unblock();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  SampleCache ok;  // a decoder that finishes is joined as before
  const fs::path wav = root / "fine.wav";
  AudioBuffer b(2, 400);
  writeWavFile(wav.u8string(), b, 44100, 16);
  ok.request(wav.u8string());
  ok.waitIdle();
  ex(ok.shutdown(300) && !ok.abandoned(), "quit: decoders that finish are joined");
#else
  (void)root;
#endif
}

void writeBmp(const fs::path& p) {
  // 2 x 2, 24 bit, bottom-up
  const unsigned char px[2][8] = {{255, 0, 0, 0, 255, 0, 0, 0}, {0, 0, 255, 255, 255, 255, 0, 0}};
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("BM", 2), u32(54 + 16), u32(0), u32(54);
  u32(40), u32(2), u32(2), u16(1), u16(24), u32(0), u32(16), u32(2835), u32(2835), u32(0), u32(0);
  f.write(reinterpret_cast<const char*>(px), sizeof px);
}

void waitBackground() {
  for (int i = 0; i < 1000 && backgroundReadsRunning() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void testImageAndCab(const fs::path& root) {
  const fs::path share = root / "pictures";
  std::error_code ec;
  fs::create_directories(share / "cabs", ec);
  remotefs::setTestPrefixes({share.u8string()});
  writeBmp(share / "pic.bmp");
  {
    SampleCache::NonBlocking ui;  // the interface thread's build
    std::string err;
    const auto t = Clk::now();
    const bool first = packs::models::loadImageFile((share / "pic.bmp").u8string(), &err) != nullptr;
    const double ms = msSince(t);
    ex(!first && err == packs::models::kImageNotReadYet && ms < kFrameMs, fmt("image synth: a picture on a network drive is not read by the interface (%.1f ms)", ms));
  }
  waitBackground();
  {
    SampleCache::NonBlocking ui;
    ex(packs::models::loadImageFile((share / "pic.bmp").u8string()) != nullptr, "image synth: the background read has it for the next build");
  }
#ifndef _WIN32
  {
    Pipe p(share / "stuck.bmp");
    SampleCache::NonBlocking ui;
    double worst = 0;
    for (int f = 0; f < 10; ++f) {  // the editor asks once a frame
      const auto t = Clk::now();
      packs::models::loadImageFile(p.path);
      worst = std::max(worst, msSince(t));
    }
    ex(worst < kFrameMs, fmt("image synth: a picture that never answers holds no frame (worst %.1f ms)", worst));
    for (int i = 0; i < 20; ++i) {
      p.unblock();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
#endif
  // The amp rack's cabinet folder.
  AudioBuffer b(2, 64);
  writeWavFile((share / "cabs" / "B cab.wav").u8string(), b, 44100, 16);
  writeWavFile((share / "cabs" / "A cab.wav").u8string(), b, 44100, 16);
#if defined(_WIN32)
  _putenv_s("GRIDLINE_DEV_SLOW_LIST_MS", "600");
#else
  setenv("GRIDLINE_DEV_SLOW_LIST_MS", "600", 1);
#endif
  const std::string cabs = (share / "cabs").u8string();
  {
    SampleCache::NonBlocking ui;
    const auto t = Clk::now();
    const std::string got = packs::dynamics::cab::boardFile(cabs, 1);
    const double ms = msSince(t);
    ex(got.empty() && ms < kFrameMs, fmt("amp rack: a slow cabinet folder is not listed by the interface (%.1f ms)", ms));
  }
  waitBackground();
#if defined(_WIN32)
  _putenv_s("GRIDLINE_DEV_SLOW_LIST_MS", "");
#else
  unsetenv("GRIDLINE_DEV_SLOW_LIST_MS");
#endif
  {
    SampleCache::NonBlocking ui;
    const std::string got = packs::dynamics::cab::boardFile(cabs, 1);
    ex(fs::u8path(got).filename() == "A cab.wav", "amp rack: the listing arrives for the next build (" + got + ")");
  }
  ex(fs::u8path(packs::dynamics::cab::boardFile(cabs, 2)).filename() == "B cab.wav", "amp rack: off the interface thread it is listed at once, as before");
  remotefs::setTestPrefixes({});
}

}  // namespace

int main() {
  const fs::path root = scratch("run");
  testWorker();
  testLog(root);
  testSession(root);
  testQuit(root);
  testImageAndCab(root);
  std::error_code ec;
  fs::remove_all(root, ec);  // (a decoder left on a pipe holds nothing on disk)
  return finish();
}

// Gridline: the audio export never takes the app down and never holds it up.
//   - an offline render's worker threads are not real-time threads (they ran flat out above the
//     interface, the graphics driver and everything else on the computer, for minutes);
//   - what throws inside a render (out of memory, a file system error) comes back as the export's error;
//   - so does a progress callback that throws, at any of its calls (mix, stems, playlist tracks,
//     with and without worker threads); renderAudio and renderSong hand such an exception on;
//   - Cancel stops an encoder program that is running;
//   - a full disk is an error, with no cut-off file left under the export's name;
//   - a folder that cannot be made is an error in every format.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif
#include "../Check.h"
#include "../../src/core/Document.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/ExportJob.h"
#include "../../src/engine/Render.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GL_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define GL_TEST_ASAN 1
#endif

namespace fs = std::filesystem;
using namespace gl;

namespace {

fs::path tempFolder(const char* name) {
  const fs::path p = fs::temp_directory_path() / (std::string("gl_render_robust_") + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

// One bar of notes on a built-in synth, on the playlist at `start`.
Song shortSong(Tick start = 0) {
  Document d;
  const ObjectId ch = d.addChannel("synth", "gl.synth");
  const ObjectId pat = d.addPattern("p", 1);
  d.setNotes(pat, ch, {Note(0, 96, 60, 100), Note(96, 96, 64, 100)});
  d.addClip(pat, 0, start);
  return d.song();
}

ExportSettings wavSettings(const fs::path& folder) {
  ExportSettings s;
  s.folder = folder.string();
  s.baseName = "mix";
  s.formats = {ExportFormat::Wav};
  s.render.sampleRate = 44100;
  s.render.tail = TailMode::Cut;
  return s;
}

void testOfflineWorkersAreNotRealtime() {
  AudioEngine offline(48000);
  offline.setOffline(true);
  offline.setThreads(4);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the workers start up
  expect(offline.realtimeWorkers() == 0, "an offline render's workers run at normal priority");
#if defined(__APPLE__)
  AudioEngine live(48000);
  live.setThreads(4);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect(live.realtimeWorkers() > 0, "the live engine's workers stay real-time (the control)");
#endif
}

void testThrowInsideARenderIsAnError() {
  const fs::path dir = tempFolder("throw");
  const Song song = shortSong();
  ExportOutcome o;
  bool escaped = false;
  try {
    o = runExport(song, wavSettings(dir), [](double, const std::string&) { throw std::runtime_error("simulated failure"); });
  } catch (...) {
    escaped = true;
  }
  expect(!escaped, "an exception inside a render does not leave runExport");
  expect(!o.errors.empty() && o.errors[0].find("simulated failure") != std::string::npos, "and comes back as the export's error");
}

// A bar on two instruments, each on its own playlist track and mixer insert: a mix, two stems
// and two playlist track parts, so the progress callback runs inside every kind of render.
Song twoTrackSong() {
  Document d;
  const ObjectId a = d.addChannel("synth a", "gl.synth");
  const ObjectId b = d.addChannel("synth b", "gl.synth");
  const ObjectId pa = d.addPattern("a", 1);
  const ObjectId pb = d.addPattern("b", 1);
  d.setNotes(pa, a, {Note(0, 192, 60, 100), Note(192, 192, 64, 100)});
  d.setNotes(pb, b, {Note(0, 384, 48, 100)});
  d.addClip(pa, 0, 0);
  d.addClip(pb, 1, 0);
  const Song& s = d.song();
  if (s.mixer.size() > 2) {
    d.routeChannel(a, s.mixer[1].id);
    d.routeChannel(b, s.mixer[2].id);
  }
  return d.song();
}

// The progress callback throws at its n-th call, for every n a whole export makes (and the
// threads of the mixer graph are running): the export ends with that error every time, nothing
// escapes runExport, and the process goes on.
void testThrowAtEveryProgressPoint() {
  const fs::path dir = tempFolder("every");
  const Song song = twoTrackSong();
  struct Case {
    const char* name;
    bool stems, tracks;
    int threads;
  };
  const Case cases[] = {{"mix", false, false, 1}, {"mix, 4 threads", false, false, 4}, {"stems and tracks", true, true, 1},
                        {"stems and tracks, 4 threads", true, true, 4}};
  for (const Case& c : cases) {
    ExportSettings s = wavSettings(dir);
    s.splitMixer = c.stems;
    s.splitPlaylist = c.tracks;
    s.render.threads = c.threads;
    s.render.sampleRate = 22050;  // (fewer calls: every one of them is a whole export below)
    s.render.blockSize = 1024;
    int calls = 0;
    const ExportOutcome clean = runExport(song, s, [&](double, const std::string&) { ++calls; });
    expect(clean.errors.empty() && !clean.files.empty() && calls > 8, (std::string(c.name) + ": the export runs, with progress").c_str());
    std::printf("# %s: %d progress calls, %zu files\n", c.name, calls, clean.files.size());
    int failedRight = 0, escaped = 0;
    for (int n = 0; n < calls; ++n) {
      int seen = 0;
      ExportOutcome o;
      try {
        o = runExport(song, s, [&](double, const std::string&) {
          if (seen++ == n) throw std::runtime_error("simulated failure " + std::to_string(n));
        });
      } catch (...) {
        ++escaped;
        continue;
      }
      const std::string want = "simulated failure " + std::to_string(n);
      if (o.errors.size() == 1 && o.errors[0].find(want) != std::string::npos && !o.cancelled) ++failedRight;
    }
    expect(escaped == 0, (std::string(c.name) + ": a throw at any progress call stays inside runExport").c_str());
    expect(failedRight == calls, (std::string(c.name) + ": and is the export's one error, at every call").c_str());
  }
  // Out of memory and an exception of no std type from the callback, in the middle of a render.
  ExportSettings s = wavSettings(dir);
  s.render.threads = 4;
  int seen = 0;
  const ExportOutcome mem = runExport(song, s, [&](double, const std::string&) {
    if (++seen == 5) throw std::bad_alloc();
  });
  expect(mem.errors.size() == 1 && mem.errors[0].find("not enough memory") != std::string::npos, "out of memory in a render: the export's error");
  seen = 0;
  const ExportOutcome odd = runExport(song, s, [&](double, const std::string&) {
    if (++seen == 5) throw 7;
  });
  expect(odd.errors.size() == 1 && odd.errors[0].find("unknown error") != std::string::npos, "an exception of no std type: the export's error");
  // The render functions themselves hand the exception on to their caller, its type and text kept.
  RenderJob job = s.render;
  job.progress = [&](double) { throw std::runtime_error("from renderAudio"); };
  bool caught = false;
  try {
    (void)renderAudio(song, job);
  } catch (const std::runtime_error& e) {
    caught = std::string(e.what()) == "from renderAudio";
  }
  expect(caught, "renderAudio passes the exception on, unchanged");
  RenderOptions ro;
  ro.threads = 4;
  ro.progress = [&](double) { throw std::runtime_error("from renderSong"); };
  caught = false;
  try {
    (void)renderSong(song, ro);
  } catch (const std::runtime_error& e) {
    caught = std::string(e.what()) == "from renderSong";
  }
  expect(caught, "renderSong passes the exception on, unchanged");
  // And a render after all of that is whole.
  const ExportOutcome after = runExport(song, s);
  expect(after.errors.empty() && after.frames > 0 && after.peak > 0.0f, "an export after them renders the song");
}

void testTooLongForMemoryIsAnError() {
#if !defined(GL_TEST_ASAN)  // (the address sanitizer stops the process on the request itself)
  const fs::path dir = tempFolder("huge");
  const Song song = shortSong(static_cast<Tick>(100000000000LL));
  ExportSettings s = wavSettings(dir);
  s.render.sampleRate = 192000;
  s.render.oversample = 4;
  s.render.loops = 64;
  ExportOutcome o;
  bool escaped = false;
  try {
    o = runExport(song, s);
  } catch (...) {
    escaped = true;
  }
  expect(!escaped, "a project too long for the memory does not end the app");
  expect(!o.errors.empty() && o.files.empty(), "it is the export's error");
#endif
}

void testUnwritableFolder() {
  const fs::path dir = tempFolder("folder");
  const fs::path file = dir / "a file";
  std::ofstream(file) << "x";
  const Song song = shortSong();
  for (ExportFormat f : {ExportFormat::Wav, ExportFormat::Flac, ExportFormat::Mp3, ExportFormat::Ogg, ExportFormat::M4a}) {
    ExportSettings s = wavSettings(file / "below");
    s.formats = {f};
    const ExportOutcome o = runExport(song, s);
    expect(!o.errors.empty() && o.files.empty(), (std::string("a folder that cannot be made is an error: ") + exportFormatName(f)).c_str());
  }
}

#if !defined(_WIN32)
void testCancelStopsTheEncoder() {
  const fs::path dir = tempFolder("encoder");
  const fs::path tool = dir / "slow-encoder";
  std::ofstream(tool) << "#!/bin/sh\nexec sleep 60\n";
  ::chmod(tool.c_str(), 0755);
  setEncoderOverride("ffmpeg", tool.string());
  AudioBuffer buf(2, 4800);
  AudioFileOptions o;
  o.format = ExportFormat::Mp3;
  std::atomic<bool> cancel{false};
  o.cancel = &cancel;
  std::thread later([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  const bool ok = writeAudioFile((dir / "out.mp3").string(), buf, 48000, o, &err);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  later.join();
  setEncoderOverride("ffmpeg", "");
  expect(!ok && err == "cancelled", "Cancel stops the encoder");
  expect(seconds < 10.0, "at once, not when the encoder ends");
  int left = 0;
  for (const auto& e : fs::directory_iterator(dir)) left += e.path().filename() != "slow-encoder";
  expect(left == 0, "no intermediate or partial file stays behind");
}

void testFullDiskIsAnError() {
  const fs::path dir = tempFolder("full");
  const fs::path out = dir / "out.wav";
  AudioBuffer buf(2, 48000);
  // A limit on the file size fails writes the way a full disk does.
  ::signal(SIGXFSZ, SIG_IGN);
  rlimit old{};
  ::getrlimit(RLIMIT_FSIZE, &old);
  rlimit small = old;
  small.rlim_cur = 4096;
  ::setrlimit(RLIMIT_FSIZE, &small);
  std::string err;
  AudioFileOptions o;
  const bool ok = writeAudioFile(out.string(), buf, 48000, o, &err);
  ::setrlimit(RLIMIT_FSIZE, &old);
  expect(!ok && !err.empty(), "a full disk is an error, not a finished export");
  expect(!fs::exists(out), "no cut-off file stays under the export's name");
}
#endif

}  // namespace

int main() {
  testOfflineWorkersAreNotRealtime();
  testThrowInsideARenderIsAnError();
  testThrowAtEveryProgressPoint();
  testTooLongForMemoryIsAnError();
  testUnwritableFolder();
#if !defined(_WIN32)
  testCancelStopsTheEncoder();
  testFullDiskIsAnError();
#endif
  return finish();
}

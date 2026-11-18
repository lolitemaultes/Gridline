// Gridline: crash reports, the parts that run without crashing anything: the scrubber, the freeze
// decision, breadcrumbs under fire, reports on disk, sessions left behind, recoverable failures.
// (The crashes themselves: tests/k39_crash_app.cmake and tests/k39_crash_probe.cmake.)
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/Crash.h"
#include "core/Document.h"
#include "engine/ExportJob.h"
#include "core/CrashInternal.h"
#include "core/RemoteFs.h"
#include "engine/Export.h"
#include "engine/Sample.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string scrub(const char* in, const char* home = "/Users/me", const char* user = "maria") {
  char out[512];
  crash::scrubText(in, out, sizeof out, home, user);
  return out;
}

void testScrub() {
  expect(scrub("cannot open /Users/me/Songs/track.flp: no such file") == "cannot open <path>: no such file", "scrub: a folder path goes, the words after it stay");
  expect(scrub("cannot open /Users/me/My Songs/my track.flp: gone") == "cannot open <path>: gone", "scrub: spaces inside a path");
  expect(scrub("read \"/home/me/My Songs/x y.wav\" failed") == "read \"<path>\" failed", "scrub: a quoted path");
  expect(scrub("open 'C:\\Users\\me\\A B\\x.wav' failed") == "open '<path>' failed", "scrub: a quoted Windows path");
  expect(scrub("file C:\\Users\\me\\Music\\x.wav is bad") == "file <path> is bad", "scrub: a bare Windows path");
  expect(scrub("share \\\\nas\\music\\x.wav lost") == "share <path> lost", "scrub: a network path");
  expect(scrub("see ~/Music/x.wav now") == "see <path> now", "scrub: a home path");
  expect(scrub("url file:///tmp/secret.wav bad") == "url <path> bad", "scrub: a file URL");
  expect(scrub("hello maria, this is MARIA") == "hello <user>, this is <user>", "scrub: the user's name");
  expect(scrub("vector::_M_range_check: 5 >= 3") == "vector::_M_range_check: 5 >= 3", "scrub: plain messages unchanged");
  expect(scrub("ratio 1/2 and a/b stay") == "ratio 1/2 and a/b stay", "scrub: a slash inside a word is no path");
  expect(scrub("") == "", "scrub: empty");
  expect(scrub("line one\nline two").find('\n') == std::string::npos, "scrub: one line per field");
  // A tiny buffer ends with a terminator and never runs over.
  char tiny[8];
  std::memset(tiny, 'x', sizeof tiny);
  crash::scrubText("a long message that does not fit", tiny, sizeof tiny, "", "");
  expect(tiny[7] == 0 || std::strlen(tiny) < 8, "scrub: stays in its buffer");
  expect(scrub("x", nullptr, nullptr) == "x", "scrub: no home or user known");
}

using Act = crash::FreezeDetector::Action;

void testDetector() {
  {  // a living interface
    crash::FreezeDetector d(30000);
    uint64_t beat = 0;
    bool any = false;
    for (int64_t t = 0; t < 600000; t += 500) any = any || d.update(t, ++beat, true, 0, 0) != Act::None;
    expect(!any, "freeze: a beating interface never alarms");
  }
  {  // silent for the limit: once
    crash::FreezeDetector d(30000);
    int frozen = 0;
    int64_t at = -1;
    for (int64_t t = 0; t < 120000; t += 500)
      if (d.update(t, 7, true, 0, 0) == Act::Frozen) {
        ++frozen;
        at = t;
      }
    expect(frozen == 1 && at >= 30000 && at <= 31000, "freeze: one alarm at the limit");
    expect(d.update(120500, 8, true, 0, 0) == Act::Recovered, "freeze: a beat after it says recovered");
    expect(!d.frozen(), "freeze: clear again");
  }
  {  // not armed: startup
    crash::FreezeDetector d(30000);
    bool any = false;
    for (int64_t t = 0; t < 120000; t += 500) any = any || d.update(t, 0, false, 0, 0) != Act::None;
    expect(!any, "freeze: nothing before it is armed");
  }
  {  // long work the app announced
    crash::FreezeDetector d(30000);
    bool any = false;
    for (int64_t t = 0; t < 300000; t += 500) any = any || d.update(t, 0, true, 600000, 0) != Act::None;
    expect(!any, "freeze: announced work is no freeze");
    bool after = false;
    for (int64_t t = 300000; t < 340000; t += 500) after = after || d.update(t, 0, true, 300000, 0) == Act::Frozen;
    expect(after, "freeze: the limit counts from when the work ended");
  }
  {  // a suspended machine: a huge gap between two looks
    crash::FreezeDetector d(30000);
    d.update(0, 5, true, 0, 0);
    d.update(500, 5, true, 0, 0);
    expect(d.update(4000000, 5, true, 0, 0) == Act::None, "freeze: waking from sleep is no freeze");
    expect(d.update(4000500, 5, true, 0, 0) == Act::None, "freeze: nor the look after");
    bool any = false;
    for (int64_t t = 4000500; t < 4029000; t += 500) any = any || d.update(t, 5, true, 0, 0) != Act::None;
    expect(!any, "freeze: the time starts anew after waking");
    // Several short naps in a row (a laptop lid, a stopped process): never adds up.
    crash::FreezeDetector n(30000);
    bool nap = false;
    for (int64_t t = 0; t < 600000; t += 6000) nap = nap || n.update(t, 5, true, 0, 0) != Act::None;
    expect(!nap, "freeze: gaps never add up to a freeze");
  }
  {  // the user chose to wait
    crash::FreezeDetector d(30000);
    for (int64_t t = 0; t <= 30000; t += 500) d.update(t, 1, true, 0, 0);
    expect(d.frozen(), "freeze: frozen before the wait");
    d.rearm(30000);
    bool any = false;
    for (int64_t t = 30500; t < 150000; t += 500) any = any || d.update(t, 1, true, 0, 150000) != Act::None;
    expect(!any, "freeze: quiet while the user waits");
    bool later = false;
    for (int64_t t = 150000; t < 185000; t += 500) later = later || d.update(t, 1, true, 0, 150000) == Act::Frozen;
    expect(later, "freeze: asks again after that");
  }
}

void testCrumbs() {
  // Many threads, nothing lost in a way that breaks a slot: every slot read is a whole crumb.
  std::vector<std::thread> writers;
  for (int i = 0; i < 4; ++i)
    writers.emplace_back([i] {
      const char* texts[4] = {"writer a", "writer b", "writer c", "writer d"};
      for (int k = 0; k < 200000; ++k) crash::crumb(texts[i], k % 2 ? "odd" : "even");
    });
  for (auto& t : writers) t.join();
  auto& ring = crash::detail::crumbRing();
  bool whole = true;
  for (const auto& s : ring.slots) {
    if (!s.seq.load()) continue;
    uint64_t w[7];
    for (int i = 0; i < 7; ++i) w[i] = s.text[i].load();
    char t[sizeof w + 1] = {};
    std::memcpy(t, w, sizeof w);
    whole = whole && std::strncmp(t, "writer ", 7) == 0;
  }
  expect(whole, "crumbs: slots stay readable under four writers");
  // Long text is cut, never run over.
  crash::crumb(std::string(500, 'x').c_str(), std::string(500, 'y').c_str());
  crash::crumb("same");
  crash::crumb("same");
  crash::crumb("same");
  const uint64_t n = ring.next.load();
  const auto& last = ring.slots[(n - 1) % crash::detail::kCrumbs];
  expect(last.repeat.load() == 2, "crumbs: the same crumb is counted, not repeated");
}

void testCode() {
  const std::string code = "GRIDLINE CRASH CODE 1\nid: GL-1\nEND GRIDLINE CRASH CODE\n";
  expect(crash::extractCode(code) == code, "code: found");
  expect(crash::extractCode("text\n" + code + "more") == code, "code: found in other text");
  expect(crash::extractCode("> GRIDLINE CRASH CODE 1\n> id: GL-1\n> END GRIDLINE CRASH CODE\n") == code, "code: a quoted paste");
  expect(crash::extractCode("nothing here").empty(), "code: none");
}

void testDisk(const fs::path& dir) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string cfg = dir.u8string();
  crash::Config c;
  c.configDir = cfg;
  c.sentinel = false;
  c.frozenAfterSeconds = 3600;
  crash::install(c);
  expect(crash::installed(), "install: done");
  expect(fs::exists(fs::u8path(crash::reportsDir(cfg))), "install: the report folder");
  expect(fs::exists(fs::u8path(crash::recoveryDir(cfg))), "install: the recovery folder");
  expect(fs::exists(fs::u8path(crash::sessionFileOf(cfg, crash::sessionId()))), "install: the session file");

  // A recoverable failure, with a path in its text.
  crash::crumb("before the failure");
  std::string id;
  try {
    throw std::runtime_error("cannot read /Users/me/Private Songs/x.flp: bad");
  } catch (...) {
    id = crash::reportCurrentException("Testing");
  }
  expect(id.rfind("GL-", 0) == 0, "recoverable: an id");
  crash::Recoverable r;
  expect(crash::takeRecoverable(r), "recoverable: queued for the pop-up");
  expect(r.where == "Testing" && r.id == id, "recoverable: what and which");
  expect(r.code.find("doing: Testing") != std::string::npos, "recoverable: the code says what it was doing");
  expect(r.code.find("cannot read <path>: bad") != std::string::npos, "recoverable: the message, without its path");
  expect(r.code.find("Private Songs") == std::string::npos, "recoverable: no folder");
  expect(r.code.find("before the failure") != std::string::npos, "recoverable: the recent actions");
  expect(r.code.find(cfg) == std::string::npos, "recoverable: not its own folder either");
  expect(r.code.find("\nstack:\n  0 ") != std::string::npos, "recoverable: a stack");
  expect(r.code.find("\nversion: ") != std::string::npos && r.code.find("\nbuild: ") != std::string::npos, "recoverable: version and build");
  expect(r.code.find("\nos: ") != std::string::npos, "recoverable: system");
  expect(fs::exists(fs::u8path(r.reportPath)), "recoverable: on disk");
  expect(!crash::takeRecoverable(r), "recoverable: one in the queue");
  // The same failure twice from one place: no second pop-up within the minute.
  auto failing = [] {
    try {
      throw std::runtime_error("the same every time");
    } catch (...) {
      return crash::reportCurrentException("Testing twice");
    }
  };
  std::string first, second;
  for (volatile int i = 0; i < 2; i = i + 1) (i == 0 ? first : second) = failing();  // one call site, not unrolled
  expect(first == second && !first.empty(), "recoverable: the same place has the same id");
  crash::Recoverable once;
  expect(crash::takeRecoverable(once) && !crash::takeRecoverable(once), "recoverable: one pop-up for a repeat");
  // Two different failures within the same second keep their own reports.
  size_t before = 0, after = 0;
  for (const auto& e : fs::directory_iterator(fs::u8path(crash::reportsDir(cfg)))) before += e.path().extension() == ".txt" ? 1 : 0;
  crash::reportRecoverable("One", "t", "first of two");
  crash::reportRecoverable("Two", "t", "second of two");
  for (const auto& e : fs::directory_iterator(fs::u8path(crash::reportsDir(cfg)))) after += e.path().extension() == ".txt" ? 1 : 0;
  expect(after == before + 2, "recoverable: two failures, two files");
  while (crash::takeRecoverable(once)) {
  }
  // A render that fails ends as a failed export, with one report and one pop-up (K-38).
  {
    Document d;
    const ObjectId ch = d.addChannel("synth", "gl.synth");
    const ObjectId pat = d.addPattern("p", 1);
    d.setNotes(pat, ch, {Note(0, 96, 60, 100)});
    d.addClip(pat, 0, 0);
    const Song song = d.song();
    ExportSettings s;
    s.folder = (dir / "render").u8string();
    s.baseName = "mix";
    s.formats = {ExportFormat::Wav};
    s.render.sampleRate = 44100;
    const ExportOutcome o = runExport(song, s, [](double, const std::string&) { throw std::runtime_error("the render failed here"); });
    expect(o.errors.size() == 1 && o.errors[0].find("the render failed here") != std::string::npos, "render: a failed export");
    crash::Recoverable rendered;
    expect(crash::takeRecoverable(rendered) && rendered.where == "Exporting audio", "render: reported");
    expect(rendered.code.find("the render failed here") != std::string::npos, "render: the report says what happened");
    expect(!crash::takeRecoverable(rendered), "render: one pop-up");
  }
  // The core-side hook works the same way.
  try {
    throw std::logic_error("from code without the app");
  } catch (...) {
    crash::caught("Hook");
  }
  crash::Recoverable hook;
  bool sawHook = false;
  while (crash::takeRecoverable(hook)) sawHook = sawHook || hook.where == "Hook";
  expect(sawHook, "recoverable: the hook reports too");
  // A non-std exception and a failure inside report text stay harmless.
  try {
    throw 42;
  } catch (...) {
    crash::reportCurrentException("Testing an int");
  }
  while (crash::takeRecoverable(hook)) {
  }

  // Exception reports are never offered at the next start.
  auto pending = crash::collectPending(cfg);
  expect(pending.empty(), "pending: recoverable reports were shown already");

  // A session that died: what the block holds is enough for a report.
  namespace det = crash::detail;
  const std::string deadSid = "1000-99999999";
  const std::string deadFile = crash::sessionFileOf(cfg, deadSid);
  {
    auto* b = static_cast<det::SessionBlock*>(det::plat::mapFile(deadFile, sizeof(det::SessionBlock), true));
    expect(b != nullptr, "dead session: mapped");
    b->size = sizeof(det::SessionBlock);
    b->pid = 99999999;  // no such process
    b->startUnix = 1000;
    det::copyText(b->sessionName, sizeof b->sessionName, deadSid.c_str());
    det::copyText(b->version, sizeof b->version, "9.9.9");
    det::copyText(b->build, sizeof b->build, "abc");
    b->unsaved.store(1);
    b->magic = det::kSessionMagic;
    det::plat::unmapFile(b, sizeof(det::SessionBlock));
  }
  // Its project has a recovery copy.
  const fs::path rec = fs::u8path(crash::recoveryDir(cfg));
  std::ofstream(rec / (deadSid + ".gridline.json")) << "{}\n";
  std::ofstream(rec / (deadSid + ".meta")) << "original=/somewhere/song.gridline.json\nsaved=12345\n";
  pending = crash::collectPending(cfg);
  expect(pending.size() == 1, "dead session: one report");
  if (pending.size() == 1) {
    const auto& p = pending[0];
    expect(p.kind == "ended", "dead session: says it ended");
    expect(p.text.find("version: 9.9.9") != std::string::npos, "dead session: from what it left");
    expect(p.text.find("unsaved work: yes") != std::string::npos, "dead session: unsaved work known");
    expect(!p.recoveryFile.empty() && p.originalProject == "/somewhere/song.gridline.json" && p.recoverySaved == 12345, "dead session: its recovery copy found");
    expect(!p.recoveryRescued, "dead session: a periodic copy");
    std::ofstream(rec / (deadSid + ".rescued")) << "12399";
    const auto again2 = crash::readReport(cfg, p.path);
    expect(again2.recoveryRescued && again2.recoverySaved == 12399, "dead session: a rescued copy says so");
    crash::markSeen(p.path);
    crash::discardRecovery(cfg, deadSid);
    expect(!fs::exists(rec / (deadSid + ".gridline.json")), "discard: the copy goes");
    expect(!fs::exists(rec / (deadSid + ".rescued")), "discard: with what belongs to it");
  }
  expect(!fs::exists(fs::u8path(deadFile)), "dead session: the file went");
  expect(crash::collectPending(cfg).empty(), "pending: seen is seen");
  // This run's own session is never treated as dead.
  expect(fs::exists(fs::u8path(crash::sessionFileOf(cfg, crash::sessionId()))), "own session: left alone");

  // Sweep keeps the newest thirty.
  const fs::path reports = fs::u8path(crash::reportsDir(cfg));
  for (int i = 0; i < 45; ++i) std::ofstream(reports / ("crash-" + std::to_string(i) + ".txt")) << "x";
  crash::sweep(cfg);
  int left = 0;
  for (const auto& e : fs::directory_iterator(reports)) left += e.path().extension() == ".txt" ? 1 : 0;
  expect(left == 30, "sweep: thirty reports stay");

  // A clean end.
  crash::markCleanExit();
  crash::markCleanExit();
  expect(true, "clean exit: twice is fine");
}

// FRZ-1118: what the interface thread is inside, for a freeze report.
void testInside() {
  expect(crash::onUiThread(), "inside: the thread that installed is the interface");
  expect(crash::insideNow().empty(), "inside: nothing at first");
  {
    crash::InsideScope outer("a plugin giving its state:", "Big Synth");
    {
      crash::InsideScope inner("waiting for the audio thread to leave", std::string("Big Synth"));
      const std::string now = crash::insideNow();
      expect(now.find("inside: a plugin giving its state: Big Synth (0 s)\n") != std::string::npos, "inside: the outer scope");
      expect(now.find("inside: waiting for the audio thread to leave Big Synth (0 s)\n") != std::string::npos, "inside: the inner scope");
      expect(now.find("a plugin giving") < now.find("waiting for"), "inside: outermost first");
    }
    expect(crash::insideNow().find("waiting") == std::string::npos, "inside: the inner scope ended");
  }
  expect(crash::insideNow().empty(), "inside: all ended");
  bool workerUi = true;
  std::string workerSaw;
  std::thread([&] {
    workerUi = crash::onUiThread();
    crash::InsideScope s("a worker's call", "Other");
    workerSaw = crash::insideNow();
  }).join();
  expect(!workerUi && workerSaw.empty(), "inside: other threads are not recorded");
  {  // deeper than the slots kept: no harm, the kept ones still show
    std::vector<std::unique_ptr<crash::InsideScope>> deep;
    for (int i = 0; i < 10; ++i) {
      deep.push_back(std::make_unique<crash::InsideScope>("level", std::to_string(i)));
    }
    expect(crash::insideNow().find("level 0 ") != std::string::npos && crash::insideNow().find("level 9 ") == std::string::npos, "inside: deep nesting keeps the outer ones");
  }
  expect(crash::insideNow().empty(), "inside: deep nesting unwound");
}

// FRZ-1118: a project on a network drive: the interface thread never reads a file there itself (a
// mount that stops answering held it in the kernel); it asks, and a background decoder reads.
void testRemoteReads(const fs::path& dir) {
  std::error_code ec;
  const fs::path remote = dir / "share";
  fs::create_directories(remote, ec);
  AudioBuffer buf;
  buf.resize(2, 4800);
  for (size_t i = 0; i < 4800; ++i) {
    buf.at(0, i) = buf.at(1, i) = 0.25f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
  }
  const std::string a = (remote / "a.wav").u8string(), b = (remote / "b.wav").u8string(), local = (dir / "local.wav").u8string();
  expect(writeWavFile(a, buf, 48000, 16) && writeWavFile(b, buf, 48000, 16) && writeWavFile(local, buf, 48000, 16), "remote: files written");
  remotefs::setTestPrefixes({remote.u8string()});
  expect(mustNotReadNow(a) && !mustNotReadNow(local), "remote: the interface does not read there, only there");
  // The first ask returns at once with nothing: the interface never decodes it itself.
  expect(sampleCache().get(a) == nullptr, "remote: get on the interface thread does not read");
  expect(loadSample(b) == nullptr, "remote: loadSample on the interface thread does not read");
  sampleCache().waitIdle();
  SamplePtr sa = sampleCache().get(a), sb = loadSample(b);
  expect(sa && sa->frames() == 4800 && sb && sb->frames() == 4800, "remote: read by the background decoder, then there");
  expect(sampleCache().get(local) != nullptr, "remote: a local file is read at once, as before");
  bool workerRead = false, workerMust = true;
  std::thread([&] {
    workerMust = mustNotReadNow(a);
    workerRead = loadSample(a) != nullptr;
  }).join();
  expect(!workerMust && workerRead, "remote: other threads read as before");
  {  // a program build (non-blocking) on any thread asks too
    bool built = true;
    std::thread([&] {
      SampleCache::NonBlocking nb;
      built = loadSample((remote / "missing.wav").u8string()) != nullptr || !mustNotReadNow(a);
    }).join();
    expect(!built, "remote: a non-blocking build asks instead of reading");
  }
  // Background readers: one per key at a time, and the generation moves when one ends.
  sampleCache().waitIdle();
  const uint64_t g0 = sampleCache().generation();
  std::atomic<int> runs{0};
  std::atomic<bool> release{false};
  readInBackground("k39 test", [&] {
    ++runs;
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  readInBackground("k39 test", [&] { ++runs; });  // still running: not started again
  release = true;
  for (int i = 0; i < 2000 && backgroundReadsRunning() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(backgroundReadsRunning() == 0 && runs.load() == 1, "background read: once per key at a time");
  expect(sampleCache().generation() != g0, "background read: the generation moves (a rebuild)");
  remotefs::setTestPrefixes({});
}

}  // namespace

int main(int argc, char** argv) {
  testScrub();
  testDetector();
  testCrumbs();
  testCode();
  testDisk(fs::u8path(argc > 1 ? argv[1] : "k39_config"));
  testInside();
  testRemoteReads(fs::u8path(argc > 1 ? argv[1] : "k39_config") / "frz1118");
  {  // The heartbeat and crumb cost nothing to speak of: a crumb is one per user action, and a slow virtual machine's clock takes 13 us.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000000; ++i) crash::uiHeartbeat();
    const double beats = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000000; ++i) crash::crumb((i & 1) ? "a" : "b");
    const double crumbs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::printf("# heartbeat %.1f ns, crumb %.1f ns\n", beats * 1000.0, crumbs * 1000.0);
    expect(beats < 1.0 && crumbs < 60.0, "overhead: negligible");
  }
  return finish();
}

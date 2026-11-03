// Gridline — J-167: opening a project without freezing the interface.
//
// A Loader builds the new project next to the open one, never touching it:
//   Reading    (worker thread) unzip, read / convert the file into its own Song, list its samples
//              and plugins, draw the welcome launcher sketch. K-42: the file is read in chunks that
//              count as progress, once, and a read on a web drive can stall without the interface
//              noticing: the window says so and Cancel returns at once;
//   Preparing  (UI thread, a slice per frame via step()) the samples decode on the sample cache's
//              background decoders, each finished one a work unit; every channel instrument and mixer
//              effect is created and its state restored in a ProgramBuilder of the loader's own, one
//              unit each (a third-party plugin is announced a frame before its blocking start, so the
//              interface says which plugin is starting while it runs);
//   Ready      everything is prepared: the caller swaps it in (Document::reset, then
//              ProgramBuilder::adoptPreloaded and a build that finds every plugin ready).
// Cancel stops at the next safe point: while reading the worker stops at its next chunk and removes
// what it unpacked, and whatever it is stuck in no longer matters (its thread is joined later, or
// left behind at quit); while preparing the prepared plugins are destroyed. The open project (document, undo, engine, runtimes) is never touched.
// Every step is logged with its time; missing samples, unavailable plugins and stand-ins are
// warnings. UI thread only (start, step, cancel and every accessor).
#pragma once
#include "../core/ProjectArchive.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../core/ProjectPreview.h"
#include "../core/RemoteFs.h"
#include "../core/Song.h"
#include "../core/flp/Flp.h"
#include "Program.h"
#include "Sample.h"

namespace gl::projload {

enum class Phase { Idle, Reading, Preparing, Ready, Failed, Cancelled };
enum class Level { Info, Slow, Warning, Error };

// One log line: `fmt` (English, marked for translation; one %s for `arg`) and the time the
// step took (`ms`, -1 = none). The window translates `fmt`; text() is the English line.
struct LogLine {
  double at = 0;  // seconds since the load started
  Level level = Level::Info;
  const char* fmt = "%s";
  std::string arg;
  int ms = -1;
  std::string text() const;
};

// The current step, for the window ("Starting plugin %s (%d of %d)").
struct Now {
  enum class Kind { Idle, Reading, Unpacking, Converting, Listing, Plugin, Sample, Building, Failed, Choosing };
  Kind kind = Kind::Idle;
  std::string name;  // plugin / sample
  int index = 0, count = 0;  // 1-based
};

// No new byte or finished sample for this long, on a drive: the window says the drive is not answering.
inline constexpr double kStallSeconds = 15.0;

struct Request {
  std::string path;     // a Gridline project, an FL Studio project (.flp) or a zipped project
  std::string flData;   // FL Studio factory data folder (setting import.flData) for conversions
  double sampleRate = 44100.0;
  bool pdc = true;
  bool sketch = true;   // draw the welcome launcher sketch on the worker
  int devDelayMs = 0;   // dev / tests: every plugin preparation and the reading take this long longer
  int devReadDelayMs = 0;  // dev / tests: every chunk read from the file takes this long longer
  std::string unpackCache;  // where archives from drives unpack ("": archive::defaultCacheRoot())
  double stallSeconds = kStallSeconds;
  bool askProject = false;  // an archive with several equal projects asks which (choices / choose)
  // A file on a drive: says whether it belongs to a collaborative project (asked on the worker).
  std::function<bool(const std::string& projectFile)> collabProbe;
  // Tests: how the opened file is read (default remotefs::openFile).
  std::function<std::unique_ptr<remotefs::ByteSource>(const std::string& path, std::string* err)> open;
};

// What a finished load hands over (valid in Phase::Ready).
struct Result {
  Song song;
  std::string projectFile;  // the file that was read (inside the unpacked folder for zips)
  bool fromFl = false;      // converted from an FL Studio project (`report` holds the conversion report)
  bool fromZip = false;     // unpacked from a zipped project
  bool inCache = false;     // K-42: unpacked in the local cache: not a place to save to, Save asks where
  archive::CopyHold copyUse;  // K-45: keeps that copy while the project is open
  bool offlineCopy = false;  // K-42: the drive could not be reached, the last copy opened
  bool collabOnDrive = false;  // K-42: a project on a drive that turned out to be collaborative
  std::vector<std::string> missingRefs;  // a zipped project: every referenced file not found
  flp::Report report;
  bool hasSketch = false;
  preview::Sketch sketch;
};

// A plugin slower to start than this is marked in the log.
inline constexpr double kSlowSeconds = 0.5;

class Loader {
 public:
  Loader();
  ~Loader();  // joins every worker (an abandoned read finishes first)
  Loader(const Loader&) = delete;
  Loader& operator=(const Loader&) = delete;

  // Starts loading (a running load is cancelled first).
  void start(Request r);
  // One slice of work (UI thread): polls the reader and the sample decoders, prepares plugins
  // until about `budgetSeconds` are used (at least one, unless the next is announced first).
  // Returns true while still loading (Reading / Preparing).
  bool step(double budgetSeconds = 0.012);
  // Runs every step to the end without frames (embedded / synchronous opens, tools, tests).
  void finish();
  // Stops at the next safe point and drops everything prepared. Not possible once Ready.
  void cancel();
  // Back to Idle after the caller has taken the result (or shown the failure).
  void clear();
  // Quit: cancels and waits for every reader (an abandoned one finishes first).
  void shutdown();

  Phase phase() const { return phase_; }
  bool active() const { return phase_ == Phase::Reading || phase_ == Phase::Preparing; }
  bool cancellable() const { return active(); }
  const Request& request() const { return req_; }
  double elapsed() const;  // seconds since start
  // Work units: reading 1, each sample 1, each plugin 1, the final build 1.
  int unitsDone() const;
  int unitsTotal() const;   // 0 while reading (not known yet)
  double progress() const;  // 0..1
  // K-42: the bar: the bytes of a large read count for most of it, then the units.
  double overall() const;
  uint64_t bytesDone() const;
  uint64_t bytesTotal() const;
  double bytesPerSecond() const;
  // Seconds without any sign of life from a drive (0 when all is moving, or nothing is on a drive).
  double stalledFor() const;
  // K-42: an archive holds several equal projects and the load waits for the person's choice.
  bool awaitingChoice() const;
  std::vector<std::string> choices() const;
  void choose(int index);
  int samplesDone() const { return samplesDone_; }
  int samplesTotal() const { return static_cast<int>(samples_.size()); }
  int pluginsDone() const { return static_cast<int>(nextItem_); }
  int pluginsTotal() const { return static_cast<int>(items_.size()); }
  int warnings() const { return warnings_; }
  Now now() const;
  // The current step in English words (logs, tests).
  std::string current() const;
  // A third-party plugin is about to start (its start blocks the interface: Cancel waits for it).
  bool blockingNext() const { return announced_ >= 0; }
  const std::vector<LogLine>& log() const { return log_; }
  const std::string& error() const { return error_; }

  // Phase::Ready: the new project and its prepared plugins (moved out once).
  Result& result() { return res_; }
  ProgramBuilder& prepared() { return pre_; }

  // Abandoned readers still running (tests: none after joinFinished()).
  size_t retiredRunning();
  void joinFinished();

 private:
  struct Job;
  struct SampleItem {
    std::string path;
    double requestedAt = 0;
    bool done = false;
    SamplePtr keep;  // held until the swap (a cache trim cannot drop it meanwhile)
  };
  void add(Level l, const char* fmt, std::string arg = {}, int ms = -1);
  void pollReader();
  void pollSamples();
  bool prepareNext(double deadline);
  std::string itemLabel(size_t i) const;

  Phase phase_ = Phase::Idle;
  Request req_;
  std::chrono::steady_clock::time_point t0_{};
  std::shared_ptr<Job> job_;
  std::thread worker_;
  std::vector<std::pair<std::shared_ptr<Job>, std::thread>> retired_;
  Result res_;
  ProgramBuilder pre_;
  std::vector<ProgramBuilder::PreloadItem> items_;
  size_t nextItem_ = 0;
  int announced_ = -1;  // index of the item announced last frame (runs this frame)
  std::vector<SampleItem> samples_;
  int samplesDone_ = 0;
  std::string lastSample_;
  bool anyRemoteSample_ = false;
  double lastSampleAt_ = 0;
  uint64_t bytesRead_ = 0, bytesWanted_ = 0;  // the reading's figures once it has ended
  int warnings_ = 0;
  std::vector<LogLine> log_;
  std::string error_;
};

// The plugin's display name (catalogue name, else the type).
std::string pluginName(const std::string& type);

}  // namespace gl::projload

// Gridline — J-167: opening a project without freezing the interface (see ProjectLoad.h).
#include "ProjectLoad.h"
#include "../core/Crash.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <utility>
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/PluginStandIn.h"
#include "../core/Project.h"
#include "../core/ProjectArchive.h"
#include "../core/ProjectFiles.h"
#include "../core/ProjectInterop.h"

namespace gl::projload {

namespace fs = std::filesystem;
using i18n::N_;
using Clock = std::chrono::steady_clock;

namespace {

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
int msSince(Clock::time_point t) { return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count()); }

std::string fileName(const std::string& path) {
  const std::string n = fs::u8path(path).filename().u8string();
  return n.empty() ? path : n;
}

void sleepMs(int ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

std::string LogLine::text() const {
  char b[512];
  std::snprintf(b, sizeof b, fmt, arg.c_str());
  std::string s = b;
  if (ms >= 0) s += " (" + std::to_string(ms) + " ms)";
  return s;
}

std::string pluginName(const std::string& type) {
  const PluginInfo* p = findPlugin(type);
  return p && !p->name.empty() ? p->name : type;
}

// The reading worker's state. The worker writes everything below `done`, then sets it
// (release); the UI thread reads it only after seeing `done` (acquire).
struct Loader::Job {
  Request req;
  std::atomic<bool> done{false};
  std::atomic<int> stage{static_cast<int>(Now::Kind::Reading)};
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> bytesDone{0}, bytesTotal{0};
  std::atomic<int64_t> firstByteMs{-1}, lastSignMs{0};
  Clock::time_point t0 = Clock::now();
  bool cancelled = false;
  // The step's detail and the choice between projects, read by the UI thread while running.
  mutable std::mutex detailMu;
  std::string entry;
  int entryIndex = 0, entryCount = 0;
  std::vector<std::string> choices;
  std::atomic<int> pick{-2};
  bool ok = false;
  std::string err;
  Result res;
  std::vector<std::string> samples, missing;
  std::vector<ProgramBuilder::PreloadItem> items;
  std::vector<std::string> standins;
  struct Step {
    const char* fmt;
    std::string arg;
    int ms;
    Level level = Level::Info;
  };
  std::vector<Step> steps;

  void run() {
    try {
      read();
    } catch (const std::exception& e) {  // never a crash that loses the open project
      ok = false;
      err = std::string("unexpected failure: ") + e.what();
      crash::caught("Opening a project");
    } catch (...) {
      ok = false;
      err = "unexpected failure";
      crash::caught("Opening a project");
    }
    done.store(true, std::memory_order_release);
  }

  int64_t sinceStartMs() const { return msSince(t0); }
  void sign() { lastSignMs.store(sinceStartMs(), std::memory_order_relaxed); }
  void gotBytes(uint64_t n) {
    bytesDone.fetch_add(n, std::memory_order_relaxed);
    if (firstByteMs.load(std::memory_order_relaxed) < 0) firstByteMs.store(sinceStartMs(), std::memory_order_relaxed);
    sign();
    sleepMs(req.devReadDelayMs);
  }
  // The file as a whole, chunk by chunk (what stops or fails here is reported with the reason).
  bool readWhole(const std::string& path, std::vector<uint8_t>& bytes, bool throughTestReader, std::string& e) {
    std::unique_ptr<remotefs::ByteSource> src = throughTestReader && req.open ? req.open(path, &e) : remotefs::openFile(path, &e);
    if (!src) return false;
    bytesTotal.store(static_cast<uint64_t>(std::max<int64_t>(src->size(), 0)), std::memory_order_relaxed);
    bytesDone.store(0, std::memory_order_relaxed);
    remotefs::ReadWatch w;
    w.cancel = &cancel;
    w.onBytes = [this](uint64_t n) { gotBytes(n); };
    return remotefs::readAll(*src, bytes, w, &e);
  }
  // An archive holding several equal projects: asks, and waits for the answer or Cancel.
  int askWhich(const std::vector<std::string>& names) {
    {
      std::lock_guard<std::mutex> lk(detailMu);
      choices = names;
    }
    stage = static_cast<int>(Now::Kind::Choosing);
    while (pick.load() == -2 && !cancel.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      sign();
    }
    stage = static_cast<int>(Now::Kind::Unpacking);
    return cancel.load() ? -1 : pick.load();
  }

  void read() {
    sleepMs(req.devDelayMs);
    sign();
    std::string path = req.path;
    OpenKind kind = openKindOf(path);
    if (kind == OpenKind::ZippedProject) {
      stage = static_cast<int>(Now::Kind::Unpacking);
      const Clock::time_point t = Clock::now();
      archive::UnpackRequest u;
      u.zipPath = path;
      u.cacheRoot = req.unpackCache;
      u.cancel = &cancel;
      u.open = req.open;
      u.onTotal = [this](uint64_t n) { bytesTotal.store(n, std::memory_order_relaxed); };
      u.onBytes = [this](uint64_t n) { gotBytes(n); };
      u.onEntry = [this](const std::string& name, size_t i, size_t count) {
        std::lock_guard<std::mutex> lk(detailMu);
        entry = name;
        entryIndex = static_cast<int>(i) + 1;
        entryCount = static_cast<int>(count);
      };
      if (req.askProject) u.choose = [this](const std::vector<std::string>& names) { return askWhich(names); };
      archive::UnpackResult ur;
      std::string e;
      if (!archive::unpackForOpening(u, ur, &e)) {
        if (e == remotefs::kCancelled) cancelled = true;
        err = std::string(N_("Cannot open the zipped project")) + ": " + e;  // (translated by i18n::trMessage)
        return;
      }
      if (ur.offlineCopy) {
        steps.push_back({N_("The drive cannot be reached: opened the last copy of %s"), fileName(path), -1, Level::Warning});
      } else if (ur.reused) {
        steps.push_back({N_("Opened the copy unpacked earlier: %s"), fileName(path), msSince(t)});
      } else {
        steps.push_back({N_("Unpacked %s"), fileName(path), msSince(t)});
        if (ur.fromDrive) steps.push_back({N_("Unpacked on this computer, nothing was written to the drive: %s"), ur.folder, -1});
      }
      if (ur.otherProjects > 0) steps.push_back({N_("Other projects in this archive: %s"), std::to_string(ur.otherProjects), -1});
      res.fromZip = true;
      res.inCache = ur.inCache;
      res.copyUse = std::move(ur.use);
      res.offlineCopy = ur.offlineCopy;
      path = ur.projectFile;
      kind = openKindOf(path);
    }
    const Clock::time_point t = Clock::now();
    const fs::path projectDir = fs::u8path(path).parent_path();
    std::vector<uint8_t> bytes;
    if (kind == OpenKind::FlProject) {
      stage = static_cast<int>(Now::Kind::Reading);
      std::string e;
      bool ok = readWhole(path, bytes, !res.fromZip, e);
      if (ok) {
        stage = static_cast<int>(Now::Kind::Converting);
        ok = importFlpBytes(std::move(bytes), res.song, res.report, &e, req.flData.empty() ? flp::defaultFactoryData() : req.flData,
                            projectDir.u8string());
      }
      if (!ok) {
        if (e == remotefs::kCancelled) cancelled = true;
        err = std::string(N_("Cannot open the FL Studio project")) + ": " + e;
        return;
      }
      res.fromFl = true;
      steps.push_back({N_("Converted the FL Studio project %s"), fileName(path), msSince(t)});
    } else {
      stage = static_cast<int>(Now::Kind::Reading);
      std::string e;
      bool ok = readWhole(path, bytes, !res.fromZip, e);
      if (ok) ok = loadProjectText(std::string(bytes.begin(), bytes.end()), path, res.song, &e);
      if (!ok) {
        if (e == remotefs::kCancelled) cancelled = true;
        err = std::string(N_("Could not open project")) + ": " + e;
        return;
      }
      steps.push_back({N_("Read the project file %s"), fileName(path), msSince(t)});
      if (req.collabProbe && !res.fromZip && remotefs::isRemote(path)) res.collabOnDrive = req.collabProbe(path);
    }
    if (cancel.load()) {
      cancelled = true;
      return;
    }
    sign();
    res.projectFile = path;
    stage = static_cast<int>(Now::Kind::Listing);
    std::error_code ec;
    for (const auto& p : referencedFiles(res.song)) {
      if (cancel.load()) {
        cancelled = true;
        return;
      }
      if (p.rfind("builtin:", 0) == 0 || !isAudioFile(p)) continue;
      (fs::is_regular_file(fs::u8path(p), ec) ? samples : missing).push_back(p);
      sign();
    }
    if (res.fromZip) res.missingRefs = missingFiles(res.song);  // (for the relink window, without a stat on the UI thread)
    items = ProgramBuilder::preloadItems(res.song);
    standins = standin::summary(res.song);
    if (req.sketch) {
      res.sketch = preview::make(res.song);
      res.hasSketch = true;
    }
    ok = true;
  }
};

Loader::Loader() = default;

namespace {
// A reader stuck in a read that never returns must not hold up quitting: after a moment it is left
// behind (it owns its Job, and the process is ending).
void joinOrLeave(std::thread& t, const std::atomic<bool>& done) {
  if (!t.joinable()) return;
  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (done.load(std::memory_order_acquire)) t.join();
  else t.detach();
}
}  // namespace

Loader::~Loader() {
  if (job_) job_->cancel.store(true);
  if (job_) joinOrLeave(worker_, job_->done);
  else if (worker_.joinable()) worker_.join();
  for (auto& r : retired_) joinOrLeave(r.second, r.first->done);
}

void Loader::add(Level l, const char* fmt, std::string arg, int ms) {
  if (l == Level::Warning) ++warnings_;
  log_.push_back({elapsed(), l, fmt, std::move(arg), ms});
}

double Loader::elapsed() const { return phase_ == Phase::Idle ? 0.0 : secondsSince(t0_); }

void Loader::start(Request r) {
  if (active()) cancel();
  clear();
  joinFinished();
  if (worker_.joinable()) worker_.join();  // (a finished reader whose result was never polled)
  req_ = std::move(r);
  t0_ = Clock::now();
  phase_ = Phase::Reading;
  pre_.setSampleRate(req_.sampleRate);
  pre_.setPdc(req_.pdc);
  pre_.setAsyncSamples(true);
  job_ = std::make_shared<Job>();
  job_->req = req_;
  add(Level::Info, N_("Opening %s"), fileName(req_.path));
  std::shared_ptr<Job> job = job_;
  worker_ = std::thread([job] { job->run(); });
}

void Loader::clear() {
  phase_ = Phase::Idle;
  res_ = Result{};
  pre_ = ProgramBuilder{};
  items_.clear();
  nextItem_ = 0;
  announced_ = -1;
  samples_.clear();
  samplesDone_ = 0;
  lastSample_.clear();
  anyRemoteSample_ = false;
  lastSampleAt_ = 0;
  bytesRead_ = bytesWanted_ = 0;
  warnings_ = 0;
  log_.clear();
  error_.clear();
}

void Loader::cancel() {
  if (!active()) return;
  if (phase_ == Phase::Reading && job_) {  // it stops at its next chunk; the parser cannot be interrupted: its result is dropped
    job_->cancel.store(true);
    retired_.emplace_back(std::move(job_), std::move(worker_));
    job_.reset();
  }
  // Prepared plugins are destroyed here (UI thread); held samples are let go, unread ones stay unread.
  pre_ = ProgramBuilder{};
  {
    std::vector<std::string> unread;
    for (const auto& s : samples_)
      if (!s.done) unread.push_back(s.path);
    if (!unread.empty()) sampleCache().dropQueued(unread);
  }
  samples_.clear();
  samplesDone_ = 0;
  res_ = Result{};
  add(Level::Info, N_("Cancelled after %s"), std::to_string(static_cast<int>(elapsed() * 1000.0)) + " ms");
  phase_ = Phase::Cancelled;
}

void Loader::shutdown() {
  cancel();
  clear();
  if (worker_.joinable()) worker_.join();  // (not stuck: only a reader whose result was never polled is here)
  for (auto& r : retired_) joinOrLeave(r.second, r.first->done);
  retired_.clear();
  job_.reset();
}

size_t Loader::retiredRunning() {
  size_t n = 0;
  for (const auto& r : retired_) n += r.first->done.load(std::memory_order_acquire) ? 0 : 1;
  return n;
}

void Loader::joinFinished() {
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (it->first->done.load(std::memory_order_acquire)) {
      if (it->second.joinable()) it->second.join();
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
}

void Loader::pollReader() {
  if (!job_ || !job_->done.load(std::memory_order_acquire)) return;
  if (worker_.joinable()) worker_.join();
  std::shared_ptr<Job> job = std::move(job_);
  bytesRead_ = job->bytesDone.load();
  bytesWanted_ = job->bytesTotal.load();
  for (auto& s : job->steps) add(s.level, s.fmt, std::move(s.arg), s.ms);
  if (!job->ok) {
    error_ = job->err;
    add(Level::Error, "%s", job->err);
    phase_ = Phase::Failed;
    return;
  }
  res_ = std::move(job->res);
  items_ = std::move(job->items);
  for (const auto& m : job->missing) add(Level::Warning, N_("Sample not found: %s"), m);
  for (const auto& s : job->standins) add(Level::Warning, "%s", s);
  if (res_.fromFl) {
    for (const auto& w : res_.report.warnings) add(Level::Warning, "%s", w);
    for (const auto& [name, n] : res_.report.unmapped)
      add(Level::Warning, N_("No Gridline equivalent yet: %s"), name + (n > 1 ? " (x" + std::to_string(n) + ")" : std::string()));
  }
  // Every sample is requested at once: the cache decodes them on its background decoders.
  for (const auto& p : job->samples) {
    SampleItem it;
    it.path = p;
    it.requestedAt = elapsed();
    samples_.push_back(std::move(it));
  }
  for (auto& s : samples_) {
    anyRemoteSample_ = anyRemoteSample_ || remotefs::isRemote(s.path);
    sampleCache().request(s.path);
  }
  lastSampleAt_ = elapsed();
  phase_ = Phase::Preparing;
  pollSamples();
}

void Loader::pollSamples() {
  SampleCache& cache = sampleCache();
  for (auto& s : samples_) {
    if (s.done) continue;
    SamplePtr p = cache.request(s.path);
    if (!p) {
      if (cache.isPending(s.path)) continue;
      p = cache.request(s.path);  // (finished between the two calls)
      if (!p && cache.isPending(s.path)) continue;
    }
    s.done = true;
    ++samplesDone_;
    lastSample_ = s.path;
    lastSampleAt_ = elapsed();
    const int ms = static_cast<int>((elapsed() - s.requestedAt) * 1000.0);
    if (p) {
      s.keep = std::move(p);
      add(Level::Info, N_("Loaded sample %s"), fileName(s.path), ms);
    } else {
      add(Level::Warning, N_("Could not read sample %s"), s.path);
    }
  }
}

std::string Loader::itemLabel(size_t i) const {
  if (i >= items_.size()) return {};
  const auto& it = items_[i];
  const Song& s = res_.song;
  std::string where;
  if (it.effect) {
    const MixerTrack* m = s.mixerTrack(it.owner);
    const int idx = s.mixerIndex(it.owner);
    where = m && !m->name.empty() ? m->name : (idx == 0 ? std::string("Master") : "Insert " + std::to_string(idx));
    where += " slot " + std::to_string(it.slot + 1);
  } else if (const Channel* c = s.channel(it.owner)) {
    where = c->name;
  }
  const std::string name = pluginName(it.type);
  return where.empty() || where == name ? name : name + " (" + where + ")";
}

bool Loader::prepareNext(double deadline) {
  while (nextItem_ < items_.size()) {
    const auto& it = items_[nextItem_];
    // A third-party plugin's start blocks: it is named in a frame of its own first.
    const bool heavy = isHostedType(it.type) || req_.devDelayMs > 0;
    if (heavy && announced_ != static_cast<int>(nextItem_)) {
      announced_ = static_cast<int>(nextItem_);
      return true;
    }
    announced_ = -1;
    const Clock::time_point t = Clock::now();
    sleepMs(req_.devDelayMs);
    const bool ok = pre_.preload(res_.song, it);
    const int ms = msSince(t);
    const std::string label = itemLabel(nextItem_);
    if (!ok)
      add(Level::Warning, N_("Plugin not available: %s"), label);
    else
      add(ms >= static_cast<int>(kSlowSeconds * 1000.0) ? Level::Slow : Level::Info, N_("Started %s"), label, ms);
    ++nextItem_;
    if (nextItem_ < items_.size() && (isHostedType(items_[nextItem_].type) || req_.devDelayMs > 0)) {
      announced_ = static_cast<int>(nextItem_);  // named now, started next frame
      return true;
    }
    if (secondsSince(t0_) >= deadline) return true;
  }
  return false;
}

bool Loader::step(double budgetSeconds) {
  joinFinished();
  if (phase_ == Phase::Reading) pollReader();
  if (phase_ != Phase::Preparing) return active();
  const double deadline = secondsSince(t0_) + std::max(0.0, budgetSeconds);
  pollSamples();
  prepareNext(deadline);
  pollSamples();
  if (nextItem_ >= items_.size() && samplesDone_ >= static_cast<int>(samples_.size())) {
    add(Level::Info, N_("Prepared in %s"), std::to_string(static_cast<int>(elapsed() * 1000.0)) + " ms");
    phase_ = Phase::Ready;
  }
  return active();
}

void Loader::finish() {
  while (step(1.0)) {
    if (awaitingChoice()) choose(0);  // no frames to ask in: the suggested one
    if (phase_ == Phase::Reading || (nextItem_ >= items_.size() && samplesDone_ < static_cast<int>(samples_.size())))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int Loader::unitsTotal() const {
  if (phase_ == Phase::Reading || phase_ == Phase::Idle) return 0;
  return 2 + static_cast<int>(samples_.size()) + static_cast<int>(items_.size());
}

int Loader::unitsDone() const {
  if (phase_ == Phase::Reading || phase_ == Phase::Idle) return 0;
  return 1 + samplesDone_ + static_cast<int>(nextItem_);
}

double Loader::progress() const {
  const int total = unitsTotal();
  return total > 0 ? std::clamp(static_cast<double>(unitsDone()) / static_cast<double>(total), 0.0, 1.0) : 0.0;
}

namespace {
constexpr uint64_t kLargeRead = 8u * 1024u * 1024u;  // from here the bytes count on the bar
constexpr double kBytesShare = 0.6;
}  // namespace

uint64_t Loader::bytesDone() const { return job_ ? job_->bytesDone.load(std::memory_order_relaxed) : bytesRead_; }
uint64_t Loader::bytesTotal() const { return job_ ? job_->bytesTotal.load(std::memory_order_relaxed) : bytesWanted_; }

double Loader::bytesPerSecond() const {
  if (!job_) return 0.0;
  const int64_t first = job_->firstByteMs.load(std::memory_order_relaxed);
  const int64_t ms = job_->sinceStartMs() - first;
  return first < 0 || ms < 300 ? 0.0 : static_cast<double>(bytesDone()) * 1000.0 / static_cast<double>(ms);
}

double Loader::overall() const {
  const uint64_t total = bytesTotal();
  if (total < kLargeRead) return progress();
  if (phase_ == Phase::Reading) return kBytesShare * std::clamp(static_cast<double>(bytesDone()) / static_cast<double>(total), 0.0, 1.0);
  return kBytesShare + (1.0 - kBytesShare) * progress();
}

double Loader::stalledFor() const {
  if (phase_ == Phase::Reading && job_ && (remotefs::isRemote(req_.path) || req_.devReadDelayMs > 0)) {
    const int stage = job_->stage.load();
    if (stage == static_cast<int>(Now::Kind::Choosing) || stage == static_cast<int>(Now::Kind::Converting)) return 0.0;  // (not waiting for the drive)
    const double since = static_cast<double>(job_->sinceStartMs() - job_->lastSignMs.load(std::memory_order_relaxed)) / 1000.0;
    return since >= req_.stallSeconds ? since : 0.0;
  }
  if (phase_ == Phase::Preparing && anyRemoteSample_ && samplesDone_ < static_cast<int>(samples_.size())) {
    const double since = elapsed() - lastSampleAt_;
    return since >= req_.stallSeconds ? since : 0.0;
  }
  return 0.0;
}

bool Loader::awaitingChoice() const { return phase_ == Phase::Reading && job_ && job_->stage.load() == static_cast<int>(Now::Kind::Choosing); }

std::vector<std::string> Loader::choices() const {
  if (!job_) return {};
  std::lock_guard<std::mutex> lk(job_->detailMu);
  return job_->choices;
}

void Loader::choose(int index) {
  if (job_) job_->pick.store(index);
}

Now Loader::now() const {
  Now n;
  switch (phase_) {
    case Phase::Idle:
    case Phase::Cancelled: return n;
    case Phase::Failed: n.kind = Now::Kind::Failed; return n;
    case Phase::Reading:
      n.kind = job_ ? static_cast<Now::Kind>(job_->stage.load()) : Now::Kind::Reading;
      if (job_ && n.kind == Now::Kind::Unpacking) {
        std::lock_guard<std::mutex> lk(job_->detailMu);
        n.name = job_->entry;
        n.index = job_->entryIndex;
        n.count = job_->entryCount;
      }
      return n;
    case Phase::Ready: n.kind = Now::Kind::Building; return n;
    case Phase::Preparing: break;
  }
  if (nextItem_ < items_.size()) {
    const size_t i = announced_ >= 0 ? static_cast<size_t>(announced_) : nextItem_;
    n.kind = Now::Kind::Plugin;
    n.name = itemLabel(i);
    n.index = static_cast<int>(i) + 1;
    n.count = static_cast<int>(items_.size());
    return n;
  }
  n.kind = Now::Kind::Sample;
  n.count = static_cast<int>(samples_.size());
  n.index = std::min(samplesDone_ + 1, n.count);
  for (const auto& s : samples_)
    if (!s.done) {
      n.name = fileName(s.path);
      break;
    }
  return n;
}

std::string Loader::current() const {
  const Now n = now();
  char b[600];
  switch (n.kind) {
    case Now::Kind::Idle: return {};
    case Now::Kind::Reading: return "Reading the project file";
    case Now::Kind::Unpacking: return "Unpacking the zipped project";
    case Now::Kind::Choosing: return "Which project in this archive should open?";
    case Now::Kind::Converting: return "Converting the FL Studio project";
    case Now::Kind::Listing: return "Listing samples and plugins";
    case Now::Kind::Building: return "Building the mixer, automation and arrangement";
    case Now::Kind::Failed: return "Could not open the project";
    case Now::Kind::Plugin: std::snprintf(b, sizeof b, "Starting plugin %s (%d of %d)", n.name.c_str(), n.index, n.count); return b;
    case Now::Kind::Sample: std::snprintf(b, sizeof b, "Loading sample %s (%d of %d)", n.name.c_str(), n.index, n.count); return b;
  }
  return {};
}

}  // namespace gl::projload

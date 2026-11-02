// Gridline: ASIO host logic and the simulated driver (K-6). Platform independent; the Windows
// driver list and COM loading are in AsioWin.cpp. See AsioHost.h.
#include "AsioHost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gl::asio {

namespace {
bool fakeDriversOn() {
  const char* e = std::getenv("GRIDLINE_FAKE_ASIO");
  return e && *e && *e != '0';
}

std::string cstr(const char* s, size_t n) {
  size_t k = 0;
  while (k < n && s[k]) ++k;
  return std::string(s, k);
}
}  // namespace

std::vector<DriverEntry> listDrivers() {
  std::vector<DriverEntry> v = listInstalledDrivers();
  if (fakeDriversOn()) {
    v.push_back({"Gridline Simulated Interface", "", true});
    v.push_back({"Gridline Simulated Stereo", "", true});
  }
  return v;
}

SimSpec simSpecFor(const std::string& name) {
  SimSpec s;
  s.name = name;
  if (name.find("Stereo") != std::string::npos) {  // a small interface: 2 x 2, 24-bit, step 64
    s.inputs = 2, s.outputs = 2;
    s.buffers = {64, 1024, 128, 64};
    s.rates = {44100, 48000};
    s.type = abi::kInt24LSB;
  }
  return s;
}

bool backendAvailable() {
#ifdef _WIN32
  return true;
#else
  return fakeDriversOn();
#endif
}

// ------------------------------------------------------------------ simulated driver

struct SimDriver::Thread {
  std::thread t;
};

SimDriver::SimDriver(SimSpec s) : spec_(std::move(s)) {}

SimDriver::~SimDriver() {
  stop();
  disposeBuffers();
}

abi::Error SimDriver::start() {
  if (infos_.empty() || !cb_) return abi::kInvalidMode;
  if (running_) return abi::kOk;
  running_ = true;
  if (spec_.threaded) {
    thread_ = std::make_unique<Thread>();
    thread_->t = std::thread([this]() { run(); });
  }
  return abi::kOk;
}

abi::Error SimDriver::stop() {
  running_ = false;
  if (thread_) {
    if (thread_->t.joinable()) thread_->t.join();
    thread_.reset();
  }
  return abi::kOk;
}

void SimDriver::run() {
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  while (running_) {
    step();
    next += std::chrono::nanoseconds(static_cast<long long>(1e9 * frames_ / std::max(1.0, spec_.rate)));
    const auto now = clock::now();
    if (next < now - std::chrono::milliseconds(200)) next = now;  // fell far behind: no catch-up burst
    std::this_thread::sleep_until(next);
  }
}

abi::Error SimDriver::getChannels(abi::Long* in, abi::Long* out) {
  if (!spec_.present) {
    *in = *out = 0;
    return abi::kNotPresent;
  }
  *in = spec_.inputs, *out = spec_.outputs;
  return abi::kOk;
}

abi::Error SimDriver::getLatencies(abi::Long* in, abi::Long* out) {
  *in = frames_ + spec_.inLatencyExtra, *out = frames_ + spec_.outLatencyExtra;
  return abi::kOk;
}

abi::Error SimDriver::getBufferSize(abi::Long* minSize, abi::Long* maxSize, abi::Long* preferred, abi::Long* granularity) {
  *minSize = spec_.buffers.minSize, *maxSize = spec_.buffers.maxSize;
  *preferred = spec_.buffers.preferred, *granularity = spec_.buffers.granularity;
  return abi::kOk;
}

abi::Error SimDriver::canSampleRate(abi::SampleRate rate) {
  for (int r : spec_.rates)
    if (std::fabs(rate - r) < 0.5) return abi::kOk;
  return abi::kNoClock;
}

abi::Error SimDriver::getSampleRate(abi::SampleRate* rate) {
  *rate = spec_.rate;
  return abi::kOk;
}

abi::Error SimDriver::setSampleRate(abi::SampleRate rate) {
  if (canSampleRate(rate) != abi::kOk) return abi::kNoClock;
  spec_.rate = rate;
  return abi::kOk;
}

abi::Error SimDriver::getChannelInfo(abi::ChannelInfo* info) {
  const int n = info->isInput ? spec_.inputs : spec_.outputs;
  if (info->channel < 0 || info->channel >= n) return abi::kInvalidParameter;
  info->isActive = abi::Bool{0};
  for (const auto& b : infos_)
    if (b.isInput == info->isInput && b.channelNum == info->channel) info->isActive = abi::Bool{1};
  info->channelGroup = 0;
  info->type = spec_.type;
  std::snprintf(info->name, sizeof info->name, "%s %d", info->isInput ? "Analog In" : "Analog Out", static_cast<int>(info->channel) + 1);
  return abi::kOk;
}

abi::Error SimDriver::createBuffers(abi::BufferInfo* infos, abi::Long count, abi::Long frames, abi::Callbacks* cb) {
  if (!infos || count <= 0 || !cb) return abi::kInvalidParameter;
  if (!bufferSizeValid(spec_.buffers, static_cast<int>(frames))) return abi::kInvalidMode;
  disposeBuffers();
  const int bytes = formatBytes(spec_.type);
  frames_ = static_cast<int>(frames);
  for (abi::Long k = 0; k < count; ++k) {
    abi::BufferInfo& b = infos[k];
    const int n = b.isInput ? spec_.inputs : spec_.outputs;
    if (b.channelNum < 0 || b.channelNum >= n) {
      disposeBuffers();
      return abi::kInvalidParameter;
    }
    for (int h = 0; h < 2; ++h) {
      mem_.emplace_back(static_cast<size_t>(bytes) * static_cast<size_t>(frames_), 0);
      b.buffers[h] = mem_.back().data();
    }
    infos_.push_back(b);
  }
  cb_ = cb;
  half_ = 0;
  return abi::kOk;
}

abi::Error SimDriver::disposeBuffers() {
  stop();
  infos_.clear();
  mem_.clear();
  cb_ = nullptr;
  return abi::kOk;
}

abi::Error SimDriver::controlPanel() {
  ++panelCalls_;
  return abi::kOk;
}

abi::Error SimDriver::outputReady() {
  if (!spec_.supportsOutputReady) return abi::kNotPresent;
  outputReadyCalls_.fetch_add(1);
  return abi::kOk;
}

void SimDriver::step() {
  if (!cb_ || infos_.empty()) return;
  // Inputs: a ramp per channel (no microphone): sample i of input c = ((pos + i + 7c) % 100 - 50) / 100.
  float tmp[256];
  for (const auto& b : infos_) {
    if (!b.isInput) continue;
    auto* dst = static_cast<unsigned char*>(b.buffers[half_]);
    const int bytes = formatBytes(spec_.type);
    for (int done = 0; done < frames_;) {
      const int n = std::min(frames_ - done, 256);
      for (int i = 0; i < n; ++i)
        tmp[i] = static_cast<float>((pos_ + done + i + 7 * b.channelNum) % 100 - 50) / 100.0f;
      fromFloat(tmp, spec_.type, dst + static_cast<size_t>(done) * static_cast<size_t>(bytes), n);
      done += n;
    }
  }
  cb_->bufferSwitch(half_, abi::Bool{1});
  half_ ^= 1;
  pos_ += frames_;
}

long SimDriver::message(abi::Long selector, abi::Long value) {
  if (!cb_ || !cb_->asioMessage) return 0;
  if (cb_->asioMessage(abi::kSelectorSupported, selector, nullptr, nullptr) != 1) return 0;
  return cb_->asioMessage(selector, value, nullptr, nullptr);
}

void SimDriver::changeRate(double rate) {
  spec_.rate = rate;
  if (cb_ && cb_->sampleRateDidChange) cb_->sampleRateDidChange(rate);
}

const void* SimDriver::buffer(int k, int half) const {
  if (k < 0 || k >= static_cast<int>(infos_.size())) return nullptr;
  return infos_[static_cast<size_t>(k)].buffers[half & 1];
}

// ------------------------------------------------------------------ host

std::atomic<Host*> Host::active_{nullptr};

bool Host::open(const Config& cfg, void* sysHandle, RenderFn fn, void* user, std::string* err) {
  close();
  DriverEntry entry;
  bool found = false;
  for (const auto& e : listDrivers())
    if (e.name == cfg.driver) entry = e, found = true;
  if (!found) {
    if (err) *err = cfg.driver.empty() ? "No ASIO driver is chosen." : "The ASIO driver \"" + cfg.driver + "\" is not installed.";
    return false;
  }
  std::string loadErr;
  std::unique_ptr<Driver> d = loadDriver(entry, &loadErr);
  if (!d) {
    if (err) *err = loadErr;
    return false;
  }
  return openDriver(std::move(d), entry.name, cfg, sysHandle, fn, user, err);
}

bool Host::openDriver(std::unique_ptr<Driver> d, const std::string& name, const Config& cfg, void* sysHandle, RenderFn fn, void* user,
                      std::string* err) {
  close();
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    close();
    return false;
  };
  if (!d) return fail("No driver.");
  driver_ = std::move(d);
  dev_ = DeviceInfo{};
  dev_.driver = name;
  Host* expected = nullptr;
  if (!active_.compare_exchange_strong(expected, this)) return fail("Another ASIO driver is open.");
  if (!driver_->init(sysHandle)) {
    const std::string m = driver_->errorMessage();
    return fail("The ASIO driver \"" + name + "\" did not start" + (m.empty() ? "." : ": " + m));
  }
  abi::Long nIn = 0, nOut = 0;
  if (driver_->getChannels(&nIn, &nOut) != abi::kOk || (nIn <= 0 && nOut <= 0)) return fail("The ASIO driver \"" + name + "\" reports no channels (is the device connected?).");
  dev_.numIn = std::min<int>(static_cast<int>(std::max<abi::Long>(0, nIn)), 512);
  dev_.numOut = std::min<int>(static_cast<int>(std::max<abi::Long>(0, nOut)), 512);
  for (int dir = 0; dir < 2; ++dir) {
    const bool input = dir == 0;
    const int n = input ? dev_.numIn : dev_.numOut;
    auto& names = input ? dev_.inNames : dev_.outNames;
    auto& types = input ? dev_.inTypes : dev_.outTypes;
    for (int k = 0; k < n; ++k) {
      abi::ChannelInfo ci{};
      ci.channel = k;
      ci.isInput = input ? 1 : 0;
      if (driver_->getChannelInfo(&ci) == abi::kOk) {
        names.push_back(cstr(ci.name, sizeof ci.name));
        types.push_back(ci.type);
      } else {
        names.emplace_back();
        types.push_back(-1);  // unknown
      }
    }
  }
  abi::Long bMin = 0, bMax = 0, bPref = 0, bGran = 0;
  if (driver_->getBufferSize(&bMin, &bMax, &bPref, &bGran) != abi::kOk) return fail("The ASIO driver \"" + name + "\" reports no buffer sizes.");
  dev_.buffers = {static_cast<int>(bMin), static_cast<int>(bMax), static_cast<int>(bPref), static_cast<int>(bGran)};
  if (dev_.buffers.preferred <= 0) dev_.buffers.preferred = std::max(1, dev_.buffers.minSize);
  if (dev_.buffers.maxSize < dev_.buffers.minSize) dev_.buffers.maxSize = dev_.buffers.minSize;
  dev_.bufferChoices = bufferSizeChoices(dev_.buffers);
  for (int r : candidateRates())
    if (driver_->canSampleRate(r) == abi::kOk) dev_.rates.push_back(r);
  abi::SampleRate cur = 0;
  driver_->getSampleRate(&cur);
  if (cfg.rate > 0 && std::fabs(cur - cfg.rate) >= 0.5 && driver_->canSampleRate(cfg.rate) == abi::kOk) {
    driver_->setSampleRate(cfg.rate);
    driver_->getSampleRate(&cur);
  }
  if (!(cur > 0)) {  // no clock reported: pick a rate the device takes
    for (int r : {48000, 44100})
      if (driver_->canSampleRate(r) == abi::kOk && driver_->setSampleRate(r) == abi::kOk) {
        cur = r;
        break;
      }
  }
  if (!(cur > 0)) return fail("The ASIO driver \"" + name + "\" has no sample clock (check the device's clock source).");
  rate_ = static_cast<int>(std::lround(cur));
  if (std::find(dev_.rates.begin(), dev_.rates.end(), rate_) == dev_.rates.end()) {
    dev_.rates.push_back(rate_);
    std::sort(dev_.rates.begin(), dev_.rates.end());
  }

  plan_ = planChannels(dev_.numIn, dev_.numOut, cfg.channels);
  if (plan_.outputs.empty()) return fail("The ASIO driver \"" + name + "\" has no outputs.");
  infos_.clear();
  types_.clear();
  for (int k : plan_.inputs) {
    abi::BufferInfo b{};
    b.isInput = 1, b.channelNum = k;
    infos_.push_back(b);
    types_.push_back(dev_.inTypes[static_cast<size_t>(k)]);
  }
  for (int k : plan_.outputs) {
    abi::BufferInfo b{};
    b.isInput = 0, b.channelNum = k;
    infos_.push_back(b);
    types_.push_back(dev_.outTypes[static_cast<size_t>(k)]);
  }
  for (abi::SampleType t : types_)
    if (!formatSupported(t)) return fail(std::string("The ASIO driver \"") + name + "\" uses a sample format Gridline cannot convert (" + formatName(t) + ").");

  frames_ = chooseBufferSize(dev_.buffers, cfg.buffer);
  cb_ = abi::Callbacks{&Host::onBufferSwitch, &Host::onRateChanged, &Host::onMessage, &Host::onBufferSwitchTimeInfo};
  abi::Error e = driver_->createBuffers(infos_.data(), static_cast<abi::Long>(infos_.size()), frames_, &cb_);
  if (e != abi::kOk && frames_ != dev_.buffers.preferred) {
    frames_ = dev_.buffers.preferred;
    e = driver_->createBuffers(infos_.data(), static_cast<abi::Long>(infos_.size()), frames_, &cb_);
  }
  if (e != abi::kOk) return fail("The ASIO driver \"" + name + "\" could not create its buffers (" + std::to_string(static_cast<long>(e)) + ").");
  created_ = true;
  const size_t nBuf = infos_.size();
  scratch_.assign(nBuf * static_cast<size_t>(frames_), 0.f);
  inPtr_.assign(plan_.inputs.size(), nullptr);
  outPtr_.assign(plan_.outputs.size(), nullptr);
  for (size_t k = 0; k < plan_.inputs.size(); ++k) inPtr_[k] = scratch_.data() + k * static_cast<size_t>(frames_);
  for (size_t k = 0; k < plan_.outputs.size(); ++k) outPtr_[k] = scratch_.data() + (plan_.inputs.size() + k) * static_cast<size_t>(frames_);
  refreshLatencies();
  postOutput_ = driver_->outputReady() == abi::kOk;
  fn_ = fn, user_ = user;
  return true;
}

bool Host::start(std::string* err) {
  if (!driver_ || !created_) {
    if (err) *err = "No ASIO driver is open.";
    return false;
  }
  if (running_) return true;
  events_.store(0);  // what the driver said while it was set up is already taken into account
  switches_.store(0);
  lastSwitches_ = 0;
  lastProgress_ = std::chrono::steady_clock::now();
  stallReported_ = false;
  running_ = true;
  live_.store(true, std::memory_order_release);
  if (driver_->start() != abi::kOk) {
    running_ = false;
    live_.store(false, std::memory_order_release);
    if (err) *err = "The ASIO driver \"" + dev_.driver + "\" did not start streaming" + (driver_->errorMessage().empty() ? "." : ": " + driver_->errorMessage());
    return false;
  }
  return true;
}

void Host::close() {
  if (driver_) {
    if (running_) driver_->stop();
    live_.store(false, std::memory_order_release);
    running_ = false;
    if (created_) driver_->disposeBuffers();
    created_ = false;
  }
  Host* self = this;
  active_.compare_exchange_strong(self, nullptr);
  driver_.reset();  // releases the driver (unloads it on Windows)
  plan_ = ChannelPlan{};
  infos_.clear();
  types_.clear();
  fn_ = nullptr, user_ = nullptr;
  frames_ = 0;
}

bool Host::controlPanel() { return driver_ && driver_->controlPanel() == abi::kOk; }

void Host::refreshLatencies() {
  abi::Long i = 0, o = 0;
  if (driver_ && driver_->getLatencies(&i, &o) == abi::kOk && i >= 0 && o >= 0) {
    inLatency_ = static_cast<int>(i), outLatency_ = static_cast<int>(o);
  } else {
    inLatency_ = outLatency_ = frames_;
  }
}

int Host::poll() {
  int ev = events_.exchange(0);
  if (!driver_) return ev;
  if (ev & Latency) refreshLatencies();
  if (ev & RateChanged) {
    abi::SampleRate r = 0;
    if (driver_->getSampleRate(&r) == abi::kOk && std::lround(r) == rate_) ev &= ~RateChanged;  // same rate: nothing to do
  }
  if (running_) {
    const auto now = std::chrono::steady_clock::now();
    const long long s = switches_.load(std::memory_order_relaxed);
    if (s != lastSwitches_) {
      lastSwitches_ = s;
      lastProgress_ = now;
      stallReported_ = false;
    } else if (!stallReported_) {
      const double period = static_cast<double>(frames_) / std::max(1, rate_);
      const double limit = std::max(2.0, 16.0 * period);
      if (std::chrono::duration<double>(now - lastProgress_).count() > limit) {
        ev |= Stalled;
        stallReported_ = true;
      }
    }
  }
  return ev;
}

void Host::process(int half) {
  const int n = frames_;
  const size_t nIn = plan_.inputs.size(), nOut = plan_.outputs.size();
  for (size_t k = 0; k < nIn; ++k) toFloat(infos_[k].buffers[half], types_[k], scratch_.data() + k * static_cast<size_t>(n), n);
  std::memset(scratch_.data() + nIn * static_cast<size_t>(n), 0, sizeof(float) * nOut * static_cast<size_t>(n));
  if (fn_) fn_(user_, inPtr_.data(), static_cast<int>(nIn), outPtr_.data(), static_cast<int>(nOut), n);
  for (size_t k = 0; k < nOut; ++k) fromFloat(outPtr_[k], types_[nIn + k], infos_[nIn + k].buffers[half], n);
  if (postOutput_) driver_->outputReady();
  switches_.fetch_add(1, std::memory_order_relaxed);
}

void Host::onBufferSwitch(abi::Long half, abi::Bool) {
  Host* h = active_.load(std::memory_order_acquire);
  if (h && h->live_.load(std::memory_order_acquire)) h->process(static_cast<int>(half & 1));
}

void Host::onRateChanged(abi::SampleRate) {
  if (Host* h = active_.load(std::memory_order_acquire)) h->events_.fetch_or(RateChanged);
}

abi::Time* Host::onBufferSwitchTimeInfo(abi::Time* t, abi::Long half, abi::Bool direct) {
  onBufferSwitch(half, direct);  // never asked for (kSupportsTimeInfo answers 0); handled anyway
  return t;
}

abi::Long Host::onMessage(abi::Long selector, abi::Long value, void*, double*) {
  Host* h = active_.load(std::memory_order_acquire);
  auto flag = [h](int e) {
    if (h) h->events_.fetch_or(e);
  };
  switch (selector) {
    case abi::kSelectorSupported:
      return value == abi::kEngineVersion || value == abi::kResetRequest || value == abi::kBufferSizeChange || value == abi::kResyncRequest ||
                     value == abi::kLatenciesChanged || value == abi::kSupportsTimeInfo || value == abi::kSupportsTimeCode || value == abi::kOverload
                 ? 1
                 : 0;
    case abi::kEngineVersion: return 2;
    case abi::kResetRequest: flag(Reset); return 1;
    case abi::kBufferSizeChange: flag(Reset); return 1;  // reopened at the new size
    case abi::kResyncRequest: flag(Resync); return 1;
    case abi::kLatenciesChanged: flag(Latency); return 1;
    case abi::kSupportsTimeInfo: return 0;
    case abi::kSupportsTimeCode: return 0;
    case abi::kOverload:
      if (h) h->overloads_.fetch_add(1, std::memory_order_relaxed);
      flag(Overload);
      return 1;
    default: return 0;
  }
}

}  // namespace gl::asio

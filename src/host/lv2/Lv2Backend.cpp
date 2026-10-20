// Gridline — LV2 hosting (format "lv2"): bundle discovery and scanning, and
// the host::Backend over one LV2 plugin instance. Uses the LV2 specification
// headers (ISC, vendored unmodified in third_party/lv2); the host side —
// Turtle reading (Turtle.h), URID map, atom sequences, worker, state, options,
// presets and plugin interfaces — is Gridline's own code (no lilv).
//
// Type id: "lv2:<plugin URI>"; the scan entry is the bundle directory.
//
// Threads. UI ("main") thread: instantiate / activate / state / presets /
// the plugin interface. Audio thread: process(), which writes control port
// values, builds the input atom sequences (MIDI, time:Position, messages from
// the interface), runs the plugin — split at parameter changes so they land
// on their sample — and reads its outputs (MIDI, control outputs, messages
// to the interface). Values cross threads through atomics and lock-free
// rings only; the audio thread never allocates or locks. The worker
// extension gets its own thread (synchronous while rendering offline).
//
// State ("lv2s1:<base64>"): every control input's value by symbol plus the
// properties the plugin's state interface stores (key / type URIs, flags,
// bytes). URID-typed values are kept as URIs; other atom bodies containing
// URIDs (atom:Object state) are stored as they are and only round-trip
// within one process.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "../Dylib.h"
#include "../PluginScan.h"
#include "Lv2Native.h"
#include "Lv2World.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/core/lv2.h"
#include "lv2/data-access/data-access.h"
#include "lv2/instance-access/instance-access.h"
#include "lv2/log/log.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/parameters/parameters.h"
#include "lv2/state/state.h"
#include "lv2/time/time.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#endif

namespace fs = std::filesystem;

// kxstudio external-ui ABI (a widget of three callbacks, a host struct),
// declared at global scope with C linkage so host and interface agree on the types.
extern "C" {
struct GridlineKxExternalWidget {
  void (*run)(GridlineKxExternalWidget*);
  void (*show)(GridlineKxExternalWidget*);
  void (*hide)(GridlineKxExternalWidget*);
};
}

namespace gl::host {

namespace {

using lv2::Port;

// ---------------------------------------------------------------- URIDs

class UridMap {
 public:
  LV2_URID map(const char* uri) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = ids_.find(uri);
    if (it != ids_.end()) return it->second;
    uris_.emplace_back(uri);
    const LV2_URID id = static_cast<LV2_URID>(uris_.size());
    ids_.emplace(uris_.back(), id);
    return id;
  }
  const char* unmap(LV2_URID id) {
    std::lock_guard<std::mutex> g(lock_);
    return id >= 1 && id <= uris_.size() ? uris_[id - 1].c_str() : nullptr;
  }

 private:
  std::mutex lock_;
  std::deque<std::string> uris_;  // stable addresses
  std::unordered_map<std::string, LV2_URID> ids_;
};
UridMap& urids() {
  static auto* m = new UridMap;  // never destroyed (plugins may unmap late)
  return *m;
}
LV2_URID_Map gMap{nullptr, [](LV2_URID_Map_Handle, const char* uri) { return urids().map(uri); }};
LV2_URID_Unmap gUnmap{nullptr, [](LV2_URID_Unmap_Handle, LV2_URID id) { return urids().unmap(id); }};
LV2_URID U(const char* uri) { return urids().map(uri); }

struct Uris {
  LV2_URID atomSequence = U(LV2_ATOM__Sequence), atomChunk = U(LV2_ATOM__Chunk), atomObject = U(LV2_ATOM__Object), atomBlank = U(LV2_ATOM__Blank),
           atomUrid = U(LV2_ATOM__URID), atomFloat = U(LV2_ATOM__Float), atomInt = U(LV2_ATOM__Int), atomLong = U(LV2_ATOM__Long),
           eventTransfer = U(LV2_ATOM__eventTransfer), midiEvent = U(LV2_MIDI__MidiEvent), timePosition = U(LV2_TIME__Position),
           timeFrame = U(LV2_TIME__frame), timeSpeed = U(LV2_TIME__speed), timeBpm = U(LV2_TIME__beatsPerMinute), timeBar = U(LV2_TIME__bar),
           timeBarBeat = U(LV2_TIME__barBeat), timeBeat = U(LV2_TIME__beat), timeBeatUnit = U(LV2_TIME__beatUnit),
           timeBeatsPerBar = U(LV2_TIME__beatsPerBar), timeFps = U(LV2_TIME__framesPerSecond), sampleRate = U(LV2_PARAMETERS__sampleRate),
           minBlock = U(LV2_BUF_SIZE__minBlockLength), maxBlock = U(LV2_BUF_SIZE__maxBlockLength),
           nominalBlock = U(LV2_BUF_SIZE__nominalBlockLength), sequenceSize = U(LV2_BUF_SIZE__sequenceSize), updateRate = U(LV2_UI__updateRate),
           scaleFactor = U(LV2_UI__scaleFactor), stateChanged = U(LV2_STATE__StateChanged), logError = U(LV2_LOG__Error),
           logWarning = U(LV2_LOG__Warning);
};
const Uris& uris() {
  static const Uris u;
  return u;
}

thread_local bool tlsInRun = false;  // inside the plugin's run() (log messages are dropped there)

int logV(LV2_Log_Handle, LV2_URID type, const char* fmt, va_list ap) {
  if (tlsInRun || (type != uris().logError && type != uris().logWarning)) return 0;
  std::fputs("[lv2] ", stderr);
  return std::vfprintf(stderr, fmt, ap);
}
int logF(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int r = logV(h, type, fmt, ap);
  va_end(ap);
  return r;
}
LV2_Log_Log gLog{nullptr, logF, logV};

// ---------------------------------------------------------------- lock-free rings

// Records of [u32 size][bytes]; one producer, one consumer (the producer
// side can be guarded by a spin flag where several threads write).
class Ring {
 public:
  explicit Ring(size_t capacity) : buf_(capacity), mask_(capacity - 1) {}
  bool write(const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0) {
    const uint32_t total = na + nb;
    const size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
    if (buf_.size() - (w - r) < total + 4u) return false;
    put(w, &total, 4);
    put(w + 4, a, na);
    if (nb) put(w + 4 + na, b, nb);
    w_.store(w + 4 + total, std::memory_order_release);
    return true;
  }
  bool empty() const { return w_.load(std::memory_order_acquire) == r_.load(std::memory_order_relaxed); }
  // Takes the next record (consumer). False when empty; `fits` is false
  // (and the record dropped) when it is larger than `cap`.
  bool pop(void* out, uint32_t cap, uint32_t& size, bool& fits) {
    const size_t r = r_.load(std::memory_order_relaxed);
    if (w_.load(std::memory_order_acquire) == r) return false;
    get(r, &size, 4);
    fits = size <= cap;
    if (fits) get(r + 4, out, size);
    r_.store(r + 4 + size, std::memory_order_release);
    return true;
  }
  void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  void put(size_t at, const void* src, size_t n) {
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) buf_[(at + i) & mask_] = s[i];
  }
  void get(size_t at, void* dst, size_t n) const {
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) d[i] = buf_[(at + i) & mask_];
  }
  std::vector<uint8_t> buf_;
  size_t mask_;
  std::atomic<size_t> w_{0}, r_{0};
};

class Semaphore {
 public:
#if defined(__APPLE__)
  Semaphore() : s_(dispatch_semaphore_create(0)) {}
  ~Semaphore() { dispatch_release(s_); }
  void post() { dispatch_semaphore_signal(s_); }
  void wait() { dispatch_semaphore_wait(s_, DISPATCH_TIME_FOREVER); }

 private:
  dispatch_semaphore_t s_;
#else
  Semaphore() { sem_init(&s_, 0, 0); }
  ~Semaphore() { sem_destroy(&s_); }
  void post() { sem_post(&s_); }
  void wait() {
    while (sem_wait(&s_) != 0) {
    }
  }

 private:
  sem_t s_;
#endif
};

// ---------------------------------------------------------------- worker

// The worker extension: run() schedules work, a worker thread does it, the
// responses go back to the plugin after the next run(). Synchronous while
// rendering offline (deterministic exports) and outside run() (state restore).
class Worker {
 public:
  Worker(const LV2_Worker_Interface* iface, LV2_Handle h) : iface_(iface), h_(h), req_(1 << 16), resp_(1 << 16), scratch_(1 << 16) {
    rt_.handle = this;
    rt_.schedule_work = [](LV2_Worker_Schedule_Handle hd, uint32_t size, const void* data) {
      auto* w = static_cast<Worker*>(hd);
      if (w->sync_.load(std::memory_order_relaxed) || !tlsInRun) return w->doWork(size, data);
      if (!w->req_.write(data, size)) return LV2_WORKER_ERR_NO_SPACE;
      w->sem_.post();
      return LV2_WORKER_SUCCESS;
    };
    sync_.store(false);
    syncSchedule_.handle = this;
    syncSchedule_.schedule_work = [](LV2_Worker_Schedule_Handle hd, uint32_t size, const void* data) {
      return static_cast<Worker*>(hd)->doWork(size, data);
    };
    thread_ = std::thread([this] { loop(); });
  }
  ~Worker() {
    quit_.store(true);
    sem_.post();
    thread_.join();
  }
  LV2_Worker_Schedule* rtSchedule() { return &rt_; }
  LV2_Worker_Schedule* syncSchedule() { return &syncSchedule_; }
  void setSync(bool on) { sync_.store(on); }
  // Audio thread (or the UI thread while processing is suspended): responses, end of run.
  void deliver() {
    uint32_t n = 0;
    bool fits = false;
    while (resp_.pop(scratch_.data(), static_cast<uint32_t>(scratch_.size()), n, fits))
      if (fits && iface_->work_response) iface_->work_response(h_, n, scratch_.data());
    if (iface_->end_run) iface_->end_run(h_);
  }

 private:
  LV2_Worker_Status doWork(uint32_t size, const void* data) {
    if (!iface_->work) return LV2_WORKER_ERR_UNKNOWN;
    return iface_->work(h_, &Worker::respond, this, size, data);
  }
  static LV2_Worker_Status respond(LV2_Worker_Respond_Handle hd, uint32_t size, const void* data) {
    auto* w = static_cast<Worker*>(hd);
    while (w->respLock_.test_and_set(std::memory_order_acquire)) {
    }
    const bool ok = w->resp_.write(data, size);
    w->respLock_.clear(std::memory_order_release);
    return ok ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
  }
  void loop() {
    std::vector<uint8_t> buf(1 << 16);
    for (;;) {
      sem_.wait();
      if (quit_.load()) return;
      uint32_t n = 0;
      bool fits = false;
      if (req_.pop(buf.data(), static_cast<uint32_t>(buf.size()), n, fits) && fits) doWork(n, buf.data());
    }
  }

  const LV2_Worker_Interface* iface_;
  LV2_Handle h_;
  Ring req_, resp_;
  std::vector<uint8_t> scratch_;
  std::atomic_flag respLock_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> quit_{false}, sync_{false};
  Semaphore sem_;
  LV2_Worker_Schedule rt_{}, syncSchedule_{};
  std::thread thread_;
};

// ---------------------------------------------------------------- state paths

char* dupString(const char* s) {
  const size_t n = std::strlen(s);
  char* p = static_cast<char*>(std::malloc(n + 1));
  if (p) std::memcpy(p, s, n + 1);
  return p;
}
LV2_State_Map_Path gMapPath{nullptr, [](LV2_State_Map_Path_Handle, const char* p) { return dupString(p); },
                            [](LV2_State_Map_Path_Handle, const char* p) { return dupString(p); }};
LV2_State_Free_Path gFreePath{nullptr, [](LV2_State_Free_Path_Handle, char* p) { std::free(p); }};
LV2_State_Make_Path gMakePath{nullptr, [](LV2_State_Make_Path_Handle, const char* p) {
                                std::error_code ec;
                                fs::path dir = fs::temp_directory_path(ec) / "gridline-lv2-state";
                                fs::path full = dir / fs::path(p).relative_path();
                                fs::create_directories(full.parent_path(), ec);
                                return dupString(full.string().c_str());
                              }};

// ---------------------------------------------------------------- binary

struct Binary {
  std::shared_ptr<Dylib> lib;
  const LV2_Descriptor* desc = nullptr;
};

Binary loadPlugin(const lv2::PluginDesc& d, std::string* err) {
  Binary b;
  b.lib = Dylib::open(d.binary, err);
  if (!b.lib) return b;
  using DescFn = const LV2_Descriptor* (*)(uint32_t);
  using LibFn = const LV2_Lib_Descriptor* (*)(const char*, const LV2_Feature* const*);
  if (auto fn = toFunction<DescFn>(b.lib->function("lv2_descriptor"))) {
    for (uint32_t i = 0; i < 4096; ++i) {
      const LV2_Descriptor* x = fn(i);
      if (!x) break;
      if (x->URI && d.uri == x->URI) {
        b.desc = x;
        break;
      }
    }
  } else if (auto lfn = toFunction<LibFn>(b.lib->function("lv2_lib_descriptor"))) {
    static const LV2_Feature* const none[] = {nullptr};
    std::string bundle = d.bundle + "/";
    if (const LV2_Lib_Descriptor* lib = lfn(bundle.c_str(), none))
      for (uint32_t i = 0; i < 4096; ++i) {
        const LV2_Descriptor* x = lib->get_plugin(lib->handle, i);
        if (!x) break;
        if (x->URI && d.uri == x->URI) {
          b.desc = x;
          break;
        }
      }
  }
  if (!b.desc && err) *err = "no descriptor for " + d.uri + " in " + d.binary;
  return b;
}

// Host features the plugin may require (instance).
bool supportedFeature(const std::string& f) {
  static const char* const k[] = {LV2_URID__map,          LV2_URID__unmap,          LV2_OPTIONS__options,        LV2_BUF_SIZE__boundedBlockLength,
                                  LV2_WORKER__schedule,    LV2_LOG__log,             LV2_STATE__loadDefaultState, LV2_STATE__mapPath,
                                  LV2_STATE__makePath,     LV2_STATE__freePath,      LV2_CORE__isLive,            LV2_CORE__inPlaceBroken,
                                  LV2_CORE__hardRTCapable, "http://lv2plug.in/ns/ext/urid#map"};
  for (const char* s : k)
    if (f == s) return true;
  return false;
}
// Host features an interface may require.
bool supportedUiFeature(const std::string& f, lv2::Ui::Kind kind) {
  static const char* const k[] = {LV2_URID__map,     LV2_URID__unmap,        LV2_UI__resize,       LV2_UI__portMap,           LV2_UI__touch,
                                  LV2_UI__idleInterface, LV2_UI__noUserResize, LV2_UI__fixedSize, LV2_INSTANCE_ACCESS_URI, LV2_DATA_ACCESS_URI,
                                  LV2_OPTIONS__options, LV2_LOG__log,         "http://lv2plug.in/ns/extensions/ui#makeResident"};
  for (const char* s : k)
    if (f == s) return true;
  if (f == LV2_UI__parent) return kind != lv2::Ui::External;
  if (f == lv2::kKxExternalHost || f == "http://lv2plug.in/ns/extensions/ui#external") return kind == lv2::Ui::External;
  return false;
}

struct ExternalHost {
  void (*ui_closed)(LV2UI_Controller);
  const char* plugin_human_id;
};

// ---------------------------------------------------------------- backend

class Lv2Backend;

class Lv2Editor final : public Editor {
 public:
  Lv2Editor(Lv2Backend* b, const lv2::Ui& ui, std::shared_ptr<Dylib> lib, const LV2UI_Descriptor* d) : b_(b), ui_(ui), lib_(std::move(lib)), d_(d) {}
  ~Lv2Editor() override;
  bool size(int& w, int& h) override;
  bool canResize() const override;
  bool attach(void* parent) override;
  void setSize(int w, int h) override;
  void idle() override;
  bool external() const override { return ui_.kind == lv2::Ui::External; }
  bool isOpen() const override { return !closed_; }
  void setContentScale(double s) override { scale_ = static_cast<float>(s); }
  void shutdown();  // cleans the interface up (before the plugin goes)

  Lv2Backend* b_;
  lv2::Ui ui_;
  bool touchSeen = false;
  std::vector<float> lastSent;  // per port: value last sent to the interface (NaN: never)

 private:
  std::shared_ptr<Dylib> lib_;
  const LV2UI_Descriptor* d_;
  LV2UI_Handle h_ = nullptr;
  LV2UI_Widget widget_ = nullptr;
  const LV2UI_Idle_Interface* idleIface_ = nullptr;
  const LV2UI_Resize* resizeIface_ = nullptr;
  bool closed_ = false;
  int reqW_ = 0, reqH_ = 0;  // size the interface asked for (ui:resize)
  float scale_ = 1.0f, rate_ = 48000.0f, updateRate_ = 30.0f;
  std::vector<uint8_t> scratch_ = std::vector<uint8_t>(1 << 16);
  // features
  LV2UI_Resize hostResize_{};
  LV2UI_Port_Map portMap_{};
  LV2UI_Touch touch_{};
  LV2_Extension_Data_Feature dataAccess_{};
  ExternalHost extHost_{};
  std::vector<LV2_Options_Option> options_;
  std::vector<LV2_Feature> feats_;
  std::vector<const LV2_Feature*> featPtrs_;
};

class Lv2Backend final : public Backend {
 public:
  static constexpr int kMaxEvents = 2048, kMaxChanges = 2048, kMaxSegments = 32;
  static constexpr uint32_t kAtomCapacity = 32768;

  static std::unique_ptr<Lv2Backend> create(const std::string& bundle, const std::string& uri, std::string* err) {
    auto d = lv2::describe(bundle, uri, err);
    if (!d) return nullptr;
    for (const auto& f : d->required)
      if (!supportedFeature(f)) {
        if (err) *err = "needs an unsupported host feature: " + f;
        return nullptr;
      }
    Binary bin = loadPlugin(*d, err);
    if (!bin.desc) return nullptr;
    std::unique_ptr<Lv2Backend> b(new Lv2Backend);
    b->d_ = d;
    b->lib_ = bin.lib;
    b->desc_ = bin.desc;
    b->layout();
    b->readParams(48000.0);
    // Instantiate now (48 kHz) so a plugin that cannot start fails here; a
    // different rate re-instantiates in prepare().
    if (!b->instantiate(48000.0, 4096, err)) return nullptr;
    return b;
  }

  ~Lv2Backend() override {
    if (editor_) editor_->shutdown();
    destroyInstance();
  }

  // ---- description ----
  const char* format() const override { return "LV2"; }
  const std::vector<ParamInfo>& params() const override { return params_; }
  bool hasEventInput() const override { return midiIn_ >= 0; }
  bool hasEventOutput() const override { return midiOutPorts_ > 0; }
  int mainInputChannels() const override { return static_cast<int>(mainIn_.size()); }
  int mainOutputChannels() const override { return outGroups_.empty() ? 0 : static_cast<int>(outGroups_[0].size()); }
  int sidechainChannels() const override { return static_cast<int>(scIn_.size()); }
  int auxOutputCount() const override { return outGroups_.size() > 1 ? static_cast<int>(outGroups_.size() - 1) : 0; }
  std::string auxOutputName(int k) const override {
    if (k < 0 || static_cast<size_t>(k + 1) >= outGroups_.size()) return Backend::auxOutputName(k);
    const Port& p = port(static_cast<uint32_t>(outGroups_[static_cast<size_t>(k + 1)][0]));
    return p.name.empty() ? Backend::auxOutputName(k) : p.name;
  }
  bool hasBypass() const override { return enabledPort_ >= 0; }
  bool instrument() const {
    return d_->instrumentClass || (midiIn_ >= 0 && mainIn_.empty());
  }
  const lv2::PluginDesc& desc() const { return *d_; }

  // ---- UI thread ----
  bool prepare(double sr, int maxBlock) override {
    maxBlock = std::max(1, maxBlock);
    if (active_ && sr == sampleRate_ && maxBlock == maxBlock_) return true;
    deactivate();
    if (!inst_ || sr != instRate_ || maxBlock > instMax_) {
      std::string saved;
      const bool had = inst_ && getState(saved);
      if (editor_ && instanceAccess_) closeEditorForReinstantiate();
      destroyInstance();
      if (!instantiate(sr, std::max(maxBlock, 512), nullptr)) return false;
      if (had) setState(saved);
    }
    sampleRate_ = sr;
    maxBlock_ = maxBlock;
    if (rangesRate_ != sr) {
      readParams(sr);
      restartFlags_ |= kRestartParamInfo;
    }
    allocate();
    activate();
    measureLatency();
    return active_;
  }
  double sampleRate() const override { return sampleRate_; }
  int maxBlock() const override { return maxBlock_; }
  bool active() const override { return active_; }
  void restart(int32_t flags) override {
    if (flags & kRestartReload) {
      const double sr = sampleRate_;
      const int mb = maxBlock_;
      deactivate();
      if (sr > 0) prepare(sr, mb);
    }
  }
  int latency() const override { return latency_.load(); }

  bool getState(std::string& data) override {
    if (!inst_) return false;
    std::string blob = "GLV2";
    auto u32 = [&blob](uint32_t v) { blob.append(reinterpret_cast<const char*>(&v), 4); };
    auto str = [&](const std::string& s) {
      u32(static_cast<uint32_t>(s.size()));
      blob += s;
    };
    u32(1);
    u32(static_cast<uint32_t>(ctlIn_.size()));
    for (uint32_t p : ctlIn_) {
      str(port(p).symbol);
      float v = cache_[p].load(std::memory_order_relaxed);
      blob.append(reinterpret_cast<const char*>(&v), 4);
    }
    std::vector<std::string> props;  // serialized properties
    if (stateIface_ && stateIface_->save) {
      struct Ctx {
        std::vector<std::string>* out;
      } ctx{&props};
      const LV2_Feature* feats[] = {&fMapPath_, &fFreePath_, &fMakePath_, nullptr};
      stateIface_->save(
          inst_,
          [](LV2_State_Handle h, uint32_t key, const void* value, size_t size, uint32_t type, uint32_t flags) -> LV2_State_Status {
            auto* c = static_cast<Ctx*>(h);
            const char* k = urids().unmap(key);
            const char* t = urids().unmap(type);
            if (!k || !t || (!value && size)) return LV2_STATE_ERR_BAD_TYPE;
            std::string body(static_cast<const char*>(value), size);
            if (type == uris().atomUrid && size == sizeof(LV2_URID)) {  // URIDs travel as URIs
              LV2_URID id = 0;
              std::memcpy(&id, value, sizeof id);
              const char* s = urids().unmap(id);
              body = s ? s : "";
            }
            std::string rec;
            auto put32 = [&rec](uint32_t v) { rec.append(reinterpret_cast<const char*>(&v), 4); };
            put32(static_cast<uint32_t>(std::strlen(k)));
            rec += k;
            put32(static_cast<uint32_t>(std::strlen(t)));
            rec += t;
            put32(flags);
            put32(static_cast<uint32_t>(body.size()));
            rec += body;
            c->out->push_back(std::move(rec));
            return LV2_STATE_SUCCESS;
          },
          &ctx, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, feats);
    }
    u32(static_cast<uint32_t>(props.size()));
    for (const auto& r : props) blob += r;
    data = "lv2s1:" + base64Encode(blob);
    return true;
  }

  bool setState(const std::string& data) override {
    if (data.compare(0, 6, "lv2s1:") != 0) return false;
    std::string blob;
    if (!base64Decode(data.substr(6), blob) || blob.size() < 12 || blob.compare(0, 4, "GLV2") != 0) return false;
    if (!inst_ && !instantiate(48000.0, 4096, nullptr)) return false;
    size_t pos = 4;
    bool ok = true;
    auto u32 = [&]() -> uint32_t {
      uint32_t v = 0;
      if (pos + 4 > blob.size()) {
        ok = false;
        return 0;
      }
      std::memcpy(&v, blob.data() + pos, 4);
      pos += 4;
      return v;
    };
    auto str = [&]() -> std::string {
      uint32_t n = u32();
      if (!ok || pos + n > blob.size()) {
        ok = false;
        return {};
      }
      std::string s = blob.substr(pos, n);
      pos += n;
      return s;
    };
    if (u32() != 1) return false;
    lv2::StateData sd;
    const uint32_t np = u32();
    for (uint32_t i = 0; ok && i < np; ++i) {
      std::string sym = str();
      float v = 0;
      if (pos + 4 > blob.size()) {
        ok = false;
        break;
      }
      std::memcpy(&v, blob.data() + pos, 4);
      pos += 4;
      sd.ports.push_back({sym, v});
    }
    const uint32_t nprops = ok ? u32() : 0;
    std::vector<uint32_t> flags;
    for (uint32_t i = 0; ok && i < nprops; ++i) {
      lv2::StateValue sv;
      sv.key = str();
      sv.type = str();
      flags.push_back(u32());
      sv.data = str();
      if (ok) sd.props.push_back(std::move(sv));
    }
    if (!ok) return false;
    applyState(sd);
    return true;
  }

  double paramNormalized(uint32_t id) const override {
    const Ctl* c = ctl(id);
    return c ? c->toNorm(cache_[id].load(std::memory_order_relaxed)) : 0.0;
  }
  void setParamNormalized(uint32_t id, double v) override {
    const Ctl* c = ctl(id);
    if (!c || active_) return;  // while processing the audio thread keeps the values
    const float plain = c->toPlain(v);
    cache_[id].store(plain, std::memory_order_relaxed);
    ctrl_[id] = plain;
  }
  std::string paramText(uint32_t id, double n) const override {
    const Ctl* c = ctl(id);
    if (!c) return {};
    const float v = c->toPlain(n);
    const Port& p = port(id);
    for (const auto& sp : p.points)
      if (std::fabs(sp.first - v) < 1e-5f * std::max(1.0f, std::fabs(v)) && !sp.second.empty()) return sp.second;
    if (p.toggled) return v >= 0.5f * (c->min + c->max) ? "On" : "Off";
    char buf[64];
    if (p.integer || p.enumeration) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(v)));
    else std::snprintf(buf, sizeof buf, std::fabs(v) >= 100 ? "%.1f" : std::fabs(v) >= 10 ? "%.2f" : "%.3f", static_cast<double>(v));
    std::string s = buf;
    if (!p.unit.empty()) s += " " + p.unit;
    return s;
  }
  void drainHostEvents(std::vector<HostEvent>& out) override {
    HostEvent e;
    while (events_.pop(e)) out.push_back(e);
    if (int32_t f = restartFlags_.exchange(0)) {
      HostEvent r;
      r.kind = HostEvent::Restart;
      r.flags = f;
      out.push_back(r);
    }
    if (dirty_.exchange(false)) {
      HostEvent d;
      d.kind = HostEvent::Dirty;
      out.push_back(d);
    }
  }
  int programCount() const override { return static_cast<int>(d_->presets.size()); }
  std::string programName(int i) const override {
    return i >= 0 && static_cast<size_t>(i) < d_->presets.size() ? d_->presets[static_cast<size_t>(i)].label : std::string();
  }
  bool selectProgram(int i) override {
    if (i < 0 || static_cast<size_t>(i) >= d_->presets.size() || !d_->model) return false;
    lv2::StateData sd;
    if (!lv2::readState(*d_->model, d_->presets[static_cast<size_t>(i)].uri, sd)) return false;
    applyState(sd);
    return true;
  }
  bool hasEditor() const override { return pickUi() >= 0; }
  std::unique_ptr<Editor> createEditor() override;
  void idle() override {}
  bool setOffline(bool offline) override {
    offline_.store(offline);
    if (worker_) worker_->setSync(offline);
    return freewheelPort_ >= 0 || worker_ != nullptr;
  }
  bool setBypass(bool on) override {
    if (enabledPort_ < 0) return false;
    bypass_.store(on);
    if (!active_) ctrl_[static_cast<size_t>(enabledPort_)] = on ? 0.0f : 1.0f;
    return true;
  }

  // ---- audio thread ----
  float* inputChannel(int ch) override {
    return ch >= 0 && static_cast<size_t>(ch) < mainIn_.size() ? audio_[mainIn_[static_cast<size_t>(ch)]].data() : nullptr;
  }
  float* sidechainChannel(int ch) override {
    return ch >= 0 && static_cast<size_t>(ch) < scIn_.size() ? audio_[scIn_[static_cast<size_t>(ch)]].data() : nullptr;
  }
  int outputChannels(int bus) const override {
    return bus >= 0 && static_cast<size_t>(bus) < outGroups_.size() ? static_cast<int>(outGroups_[static_cast<size_t>(bus)].size()) : 0;
  }
  const float* outputChannel(int bus, int ch) override {
    if (bus < 0 || static_cast<size_t>(bus) >= outGroups_.size()) return nullptr;
    const auto& g = outGroups_[static_cast<size_t>(bus)];
    return ch >= 0 && static_cast<size_t>(ch) < g.size() ? audio_[static_cast<size_t>(g[static_cast<size_t>(ch)])].data() : nullptr;
  }
  bool addParamChange(uint32_t id, double v, int offset) override {
    const Ctl* c = ctl(id);
    if (!c) return true;
    if (changeCount_ >= kMaxChanges) return false;
    int i = changeCount_++;
    while (i > 0 && changes_[i - 1].offset > offset) {
      changes_[i] = changes_[i - 1];
      --i;
    }
    changes_[i] = {std::max(0, offset), id, c->toPlain(v)};
    return true;
  }
  void addEvent(const NoteEvent& n) override {
    if (midiIn_ < 0 || eventCount_ >= kMaxEvents) return;
    MidiEv e;
    e.offset = std::max(0, n.offset);
    switch (n.type) {
      case NoteEvent::NoteOn:
      case NoteEvent::NoteOff: {
        const bool on = n.type == NoteEvent::NoteOn;
        e.data[0] = static_cast<uint8_t>((on ? 0x90 : 0x80) | (n.channel & 0x0F));
        e.data[1] = static_cast<uint8_t>(std::clamp<int>(n.key, 0, 127));
        e.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), on ? 1 : 0, 127));
        e.size = 3;
        break;
      }
      case NoteEvent::Midi: {
        std::memcpy(e.data, n.midi, 3);
        const uint8_t st = n.midi[0] & 0xF0;
        e.size = st == 0xC0 || st == 0xD0 ? 2 : 3;
        if (n.midi[0] < 0x80 || n.midi[0] >= 0xF0) return;  // channel messages only
        break;
      }
      case NoteEvent::Tuning: return;
    }
    int i = eventCount_++;
    while (i > 0 && events2_[i - 1].offset > e.offset) {
      events2_[i] = events2_[i - 1];
      --i;
    }
    events2_[i] = e;
  }
  void clearInput() override {
    eventCount_ = 0;
    changeCount_ = 0;
  }
  bool popMidiOut(MidiOut& m) override { return midiOut_.pop(m); }

  bool process(int n, const Transport& t) override {
    midiOut_.clear();
    if (!active_ || n <= 0 || n > maxBlock_) {
      for (const auto& g : outGroups_)
        for (int p : g) std::memset(audio_[static_cast<size_t>(p)].data(), 0, sizeof(float) * static_cast<size_t>(std::max(0, std::min(n, maxBlock_))));
      clearInput();
      return false;
    }
    tlsInRun = true;
    // Interface → plugin: control values and atom messages (frame 0 of the block).
    uiMsgBytes_ = 0;
    uiMsgCount_ = 0;
    uint32_t sz = 0;
    bool fits = false;
    while (uiToDsp_.pop(uiMsg_.data() + uiMsgBytes_, static_cast<uint32_t>(uiMsg_.size() - uiMsgBytes_), sz, fits)) {
      uint8_t* dst = uiMsg_.data() + uiMsgBytes_;
      if (!fits || sz < 8) continue;
      uint32_t portIdx = 0, kind = 0;
      std::memcpy(&portIdx, dst, 4);
      std::memcpy(&kind, dst + 4, 4);
      if (kind == 0 && sz >= 12 && portIdx < ctrl_.size()) {
        float v = 0;
        std::memcpy(&v, dst + 8, 4);
        ctrl_[portIdx] = v;
        cache_[portIdx].store(v, std::memory_order_relaxed);
      } else if (kind == 1 && uiMsgCount_ < kMaxUiMsgs) {
        uiMsgs_[uiMsgCount_++] = {portIdx, static_cast<uint32_t>(uiMsgBytes_ + 8), sz - 8};
        uiMsgBytes_ += sz;
      }
    }
    if (enabledPort_ >= 0) ctrl_[static_cast<size_t>(enabledPort_)] = bypass_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    if (freewheelPort_ >= 0) ctrl_[static_cast<size_t>(freewheelPort_)] = offline_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (bpmPort_ >= 0) ctrl_[static_cast<size_t>(bpmPort_)] = static_cast<float>(t.tempo);
    if (speedPort_ >= 0) ctrl_[static_cast<size_t>(speedPort_)] = t.playing ? 1.0f : 0.0f;
    const bool posChanged = firstBlock_ || t.changed || t.tempo != lastTempo_ || t.playing != lastPlaying_ || t.tsNum != lastNum_ ||
                            t.tsDen != lastDen_ || (t.playing && t.projectSamples != expectFrame_);
    firstBlock_ = false;
    lastTempo_ = t.tempo;
    lastPlaying_ = t.playing;
    lastNum_ = t.tsNum;
    lastDen_ = t.tsDen;
    expectFrame_ = t.projectSamples + (t.playing ? n : 0);

    int ci = 0, segs = 0;
    for (int start = 0; start < n;) {
      while (ci < changeCount_ && changes_[ci].offset <= start) applyChange(changes_[ci++]);
      int end = n;
      if (ci < changeCount_) {
        if (segs + 1 < kMaxSegments) {
          end = std::min(n, changes_[ci].offset);
        } else {
          while (ci < changeCount_) applyChange(changes_[ci++]);
        }
      }
      runSegment(start, end - start, segs == 0, end == n, posChanged, t);
      start = end;
      ++segs;
    }
    clearInput();
    if (latencyPort_ >= 0) {
      const int lat = std::max(0, static_cast<int>(std::lround(ctrl_[static_cast<size_t>(latencyPort_)])));
      if (lat != latency_.load(std::memory_order_relaxed)) {
        latency_.store(lat);
        restartFlags_ |= kRestartLatency;
      }
    }
    tlsInRun = false;
    return true;
  }

  // ---- interface (UI thread) ----
  int pickUi() const {
    int best = -1, bestRank = 0;
    for (size_t i = 0; i < d_->uis.size(); ++i) {
      const lv2::Ui& u = d_->uis[i];
#if defined(__APPLE__)
      const int rank = u.kind == lv2::Ui::Cocoa ? 3 : u.kind == lv2::Ui::External ? 1 : 0;
#else
      const int rank = u.kind == lv2::Ui::X11 ? 3 : u.kind == lv2::Ui::External ? 1 : 0;
#endif
      if (rank == 0) continue;
      bool ok = true;
      for (const auto& f : u.required) ok = ok && supportedUiFeature(f, u.kind);
      if (ok && rank > bestRank) best = static_cast<int>(i), bestRank = rank;
    }
    return best;
  }
  void editorGone(Lv2Editor* e) {
    if (editor_ == e) {
      editor_ = nullptr;
      uiActive_.store(false);
    }
  }
  void uiStarted(Lv2Editor* e) {
    dspToUi_.clear();
    editor_ = e;
    uiActive_.store(true);
  }
  // The interface wrote to a port.
  void uiWrite(Lv2Editor* e, uint32_t portIdx, uint32_t size, uint32_t protocol, const void* buffer) {
    if (portIdx >= d_->ports.size() || !buffer) return;
    const Port& p = port(portIdx);
    if (protocol == 0) {
      if (p.type != Port::Control || !p.input || size != sizeof(float)) return;
      float v = 0;
      std::memcpy(&v, buffer, sizeof v);
      if (!std::isfinite(v)) return;
      cache_[portIdx].store(v, std::memory_order_relaxed);
      if (!active_) ctrl_[portIdx] = v;
      if (portIdx < e->lastSent.size()) e->lastSent[portIdx] = v;
      uint32_t hdr[3] = {portIdx, 0, 0};
      std::memcpy(&hdr[2], &v, 4);
      uiToDsp_.write(hdr, sizeof hdr);
      if (const Ctl* c = ctl(portIdx); c && c->param) {
        events_.push({HostEvent::PerformEdit, portIdx, c->toNorm(v), 0});
        if (!e->touchSeen) events_.push({HostEvent::EndEdit, portIdx, 0, 0});
      }
    } else if (protocol == uris().eventTransfer) {
      if (p.type != Port::Atom || !p.input || size < sizeof(LV2_Atom)) return;
      uint32_t hdr[2] = {portIdx, 1};
      uiToDsp_.write(hdr, sizeof hdr, buffer, size);
    }
  }
  void uiTouch(uint32_t portIdx, bool grabbed) {
    if (const Ctl* c = ctl(portIdx); c && c->param) events_.push({grabbed ? HostEvent::BeginEdit : HostEvent::EndEdit, portIdx, 0, 0});
  }
  // Plugin → interface: atom messages since the last call.
  template <class F>
  void uiMessages(std::vector<uint8_t>& scratch, F&& f) {
    uint32_t sz = 0;
    bool fits = false;
    while (dspToUi_.pop(scratch.data(), static_cast<uint32_t>(scratch.size()), sz, fits)) {
      if (!fits || sz < 4 + sizeof(LV2_Atom)) continue;
      uint32_t portIdx = 0;
      std::memcpy(&portIdx, scratch.data(), 4);
      f(portIdx, scratch.data() + 4, sz - 4);
    }
  }
  float portValue(uint32_t p) const { return p < d_->ports.size() ? cache_[p].load(std::memory_order_relaxed) : 0.0f; }
  int portIndex(const char* symbol) const {
    for (const auto& p : d_->ports)
      if (p.symbol == symbol) return static_cast<int>(p.index);
    return -1;
  }
  LV2_Handle instance() const { return inst_; }
  const LV2_Descriptor* descriptor() const { return desc_; }
  bool resendAll = false;  // the interface gets every value again (state / preset loaded)

 private:
  struct Ctl {
    float min = 0, max = 1, def = 0;
    bool log = false, integer = false, toggle = false;
    std::vector<float> points;  // enumeration values
    bool param = false;         // exposed as a host parameter
    double toNorm(float v) const {
      if (!points.empty()) {
        size_t best = 0;
        for (size_t i = 1; i < points.size(); ++i)
          if (std::fabs(points[i] - v) < std::fabs(points[best] - v)) best = i;
        return points.size() > 1 ? static_cast<double>(best) / static_cast<double>(points.size() - 1) : 0.0;
      }
      if (max <= min) return 0;
      if (log) return std::clamp(std::log(std::max(v, min) / min) / std::log(max / min), 0.0f, 1.0f);
      return std::clamp((static_cast<double>(v) - min) / (static_cast<double>(max) - min), 0.0, 1.0);
    }
    float toPlain(double n) const {
      n = std::clamp(n, 0.0, 1.0);
      if (!points.empty()) return points[static_cast<size_t>(std::lround(n * static_cast<double>(points.size() - 1)))];
      if (toggle) return n >= 0.5 ? max : min;
      double v = log ? min * std::pow(static_cast<double>(max) / min, n) : min + n * (static_cast<double>(max) - min);
      if (integer) v = std::round(v);
      return static_cast<float>(std::clamp(v, static_cast<double>(min), static_cast<double>(max)));
    }
  };
  struct MidiEv {
    int offset = 0;
    uint8_t data[3] = {0, 0, 0};
    uint8_t size = 3;
  };
  struct Change {
    int offset = 0;
    uint32_t port = 0;
    float value = 0;
  };
  struct UiMsg {
    uint32_t port, at, size;
  };
  static constexpr int kMaxUiMsgs = 256;

  Lv2Backend() : uiToDsp_(1 << 16), dspToUi_(1 << 17) {
    fMapPath_ = {LV2_STATE__mapPath, &gMapPath};
    fFreePath_ = {LV2_STATE__freePath, &gFreePath};
    fMakePath_ = {LV2_STATE__makePath, &gMakePath};
    events2_.reset(new MidiEv[kMaxEvents]);
    changes_.reset(new Change[kMaxChanges]);
    uiMsg_.resize(1 << 16);
    uiMsgs_.reset(new UiMsg[kMaxUiMsgs]);
  }

  const Port& port(uint32_t i) const { return d_->ports[i]; }
  const Ctl* ctl(uint32_t id) const { return id < ctls_.size() && ctls_[id] ? ctls_[id].get() : nullptr; }

  // Port roles (once, from the description).
  void layout() {
    const size_t n = d_->ports.size();
    ctrl_.assign(n, 0.0f);
    cache_.reset(new std::atomic<float>[std::max<size_t>(1, n)]);
    std::vector<int> outs;
    for (const Port& p : d_->ports) {
      const uint32_t i = p.index;
      if (p.type == Port::Audio) {
        if (p.input) (p.sideChain ? scIn_ : mainIn_).push_back(i);
        else outs.push_back(static_cast<int>(i));
      } else if (p.type == Port::Control) {
        if (p.input) {
          ctlIn_.push_back(i);
          if (p.designation == std::string(lv2::kLv2) + "enabled") enabledPort_ = static_cast<int>(i);
          else if (p.designation == std::string(lv2::kLv2) + "freeWheeling") freewheelPort_ = static_cast<int>(i);
          else if (p.designation == LV2_TIME__beatsPerMinute) bpmPort_ = static_cast<int>(i);
          else if (p.designation == LV2_TIME__speed) speedPort_ = static_cast<int>(i);
        } else {
          ctlOut_.push_back(i);
          if (p.reportsLatency && latencyPort_ < 0) latencyPort_ = static_cast<int>(i);
        }
      } else if (p.type == Port::Atom) {
        (p.input ? atomIn_ : atomOut_).push_back(i);
        if (p.input && p.midi && (midiIn_ < 0 || (p.controlDesignated && !port(static_cast<uint32_t>(midiIn_)).controlDesignated)))
          midiIn_ = static_cast<int>(i);
        if (!p.input && p.midi) ++midiOutPorts_;
        if (p.input && p.timePosition) timePorts_ = true;
      }
    }
    // Output buses: port groups when declared (ungrouped ports alone),
    // otherwise pairs; the first is the main output.
    const bool grouped = std::any_of(outs.begin(), outs.end(), [&](int i) { return !port(static_cast<uint32_t>(i)).group.empty(); });
    std::vector<std::string> keys;
    for (size_t k = 0; k < outs.size(); ++k) {
      const std::string& g = port(static_cast<uint32_t>(outs[k])).group;
      if (!grouped) {
        if (k % 2 == 0) outGroups_.push_back({});
        outGroups_.back().push_back(outs[k]);
        continue;
      }
      auto it = g.empty() ? keys.end() : std::find(keys.begin(), keys.end(), g);
      if (it == keys.end()) {
        keys.push_back(g.empty() ? "#" + std::to_string(k) : g);
        outGroups_.push_back({outs[k]});
      } else {
        outGroups_[static_cast<size_t>(it - keys.begin())].push_back(outs[k]);
      }
    }
    atomBuf_.resize(n);
    for (uint32_t i : atomIn_) atomBuf_[i].assign((std::max(kAtomCapacity, port(i).minimumSize) + 7) / 8, 0);
    for (uint32_t i : atomOut_) atomBuf_[i].assign((std::max(kAtomCapacity, port(i).minimumSize) + 7) / 8, 0);
  }

  void readParams(double sr) {
    rangesRate_ = sr;
    params_.clear();
    ctls_.clear();
    ctls_.resize(d_->ports.size());
    for (uint32_t i : ctlIn_) {
      const Port& p = port(i);
      auto c = std::make_unique<Ctl>();
      const float scale = p.sampleRate ? static_cast<float>(sr) : 1.0f;
      c->toggle = p.toggled;
      c->min = p.hasMin ? p.min * scale : 0.0f;
      c->max = p.hasMax ? p.max * scale : (p.toggled ? 1.0f : c->min + 1.0f);
      if (c->max <= c->min) c->max = c->min + 1.0f;
      c->def = std::clamp(p.hasDef ? p.def * scale : c->min, c->min, c->max);
      c->integer = p.integer || p.enumeration;
      c->log = p.logarithmic && c->min > 0 && c->max > c->min;
      if (p.enumeration && p.points.size() >= 2)
        for (const auto& sp : p.points) c->points.push_back(sp.first);
      ParamInfo pi;
      pi.id = i;
      pi.title = p.name;
      pi.units = p.unit;
      if (c->toggle) pi.stepCount = 1;
      else if (!c->points.empty()) pi.stepCount = static_cast<int>(c->points.size() - 1);
      else if (c->integer && c->max - c->min >= 1 && c->max - c->min <= 4096) pi.stepCount = static_cast<int>(std::lround(c->max - c->min));
      pi.def = c->toNorm(c->def);
      const bool designated = !p.designation.empty();
      pi.flags = p.notAutomatic ? 0 : kParamAutomatable;
      if (static_cast<int>(i) == enabledPort_) pi.flags |= kParamBypass;
      else if (designated || p.notOnGui) pi.flags |= kParamHidden;
      c->param = static_cast<int>(i) != enabledPort_ && !designated;
      params_.push_back(std::move(pi));
      ctls_[i] = std::move(c);
    }
    indexParams();
  }

  bool instantiate(double sr, int maxBlock, std::string* err) {
    instRate_ = sr;
    instMax_ = maxBlock;
    minBlock_ = 1;
    seqSize_ = static_cast<int32_t>(kAtomCapacity);
    const Uris& u = uris();
    options_ = {
        {LV2_OPTIONS_INSTANCE, 0, u.sampleRate, sizeof(float), u.atomFloat, &optRate_},
        {LV2_OPTIONS_INSTANCE, 0, u.minBlock, sizeof(int32_t), u.atomInt, &minBlock_},
        {LV2_OPTIONS_INSTANCE, 0, u.maxBlock, sizeof(int32_t), u.atomInt, &instMax_},
        {LV2_OPTIONS_INSTANCE, 0, u.nominalBlock, sizeof(int32_t), u.atomInt, &instMax_},
        {LV2_OPTIONS_INSTANCE, 0, u.sequenceSize, sizeof(int32_t), u.atomInt, &seqSize_},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    optRate_ = static_cast<float>(sr);
    workerSlot_ = std::make_unique<LV2_Worker_Schedule>();  // filled once the worker exists
    workerSlot_->handle = this;
    workerSlot_->schedule_work = [](LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
      auto* self = static_cast<Lv2Backend*>(h);
      LV2_Worker_Schedule* s = self->worker_ ? self->worker_->rtSchedule() : nullptr;
      return s ? s->schedule_work(s->handle, size, data) : LV2_WORKER_ERR_UNKNOWN;
    };
    feats_ = {
        {LV2_URID__map, &gMap},
        {LV2_URID__unmap, &gUnmap},
        {LV2_OPTIONS__options, options_.data()},
        {LV2_BUF_SIZE__boundedBlockLength, nullptr},
        {LV2_WORKER__schedule, workerSlot_.get()},
        {LV2_LOG__log, &gLog},
        {LV2_STATE__loadDefaultState, nullptr},
        {LV2_STATE__mapPath, &gMapPath},
        {LV2_STATE__makePath, &gMakePath},
        {LV2_STATE__freePath, &gFreePath},
        {LV2_CORE__isLive, nullptr},
    };
    featPtrs_.clear();
    for (auto& f : feats_) featPtrs_.push_back(&f);
    featPtrs_.push_back(nullptr);
    const std::string bundle = d_->bundle + "/";
    inst_ = desc_->instantiate(desc_, sr, bundle.c_str(), featPtrs_.data());
    if (!inst_) {
      if (err) *err = "the plugin refused to start (instantiate failed)";
      return false;
    }
    stateIface_ = desc_->extension_data ? static_cast<const LV2_State_Interface*>(desc_->extension_data(LV2_STATE__interface)) : nullptr;
    auto* wi = desc_->extension_data ? static_cast<const LV2_Worker_Interface*>(desc_->extension_data(LV2_WORKER__interface)) : nullptr;
    if (wi) {
      worker_ = std::make_unique<Worker>(wi, inst_);
      worker_->setSync(offline_.load());
    }
    instanceAccess_ = false;
    for (const auto& ui : d_->uis)
      for (const auto& f : ui.required) instanceAccess_ = instanceAccess_ || f == LV2_INSTANCE_ACCESS_URI;
    // Control ports at their defaults (or the values the instance had), atom ports wired.
    for (uint32_t i : ctlIn_) {
      const Ctl* c = ctl(i);
      ctrl_[i] = c ? c->def : port(i).def;
      if (static_cast<int>(i) == enabledPort_) ctrl_[i] = 1.0f;
      cache_[i].store(ctrl_[i], std::memory_order_relaxed);
    }
    for (uint32_t i : ctlOut_) cache_[i].store(0.0f, std::memory_order_relaxed);
    for (uint32_t i = 0; i < ctrl_.size(); ++i)
      if (port(i).type == Port::Control) desc_->connect_port(inst_, i, &ctrl_[i]);
    for (uint32_t i : atomIn_) desc_->connect_port(inst_, i, atomBuf_[i].data());
    for (uint32_t i : atomOut_) desc_->connect_port(inst_, i, atomBuf_[i].data());
    if (!allocate()) return false;
    lv2_atom_forge_init(&forge_, &gMap);
    if (d_->defaultState && d_->model) {
      lv2::StateData sd;
      if (lv2::readState(*d_->model, d_->uri, sd)) applyState(sd);
    }
    return true;
  }

  void destroyInstance() {
    deactivate();
    worker_.reset();
    if (inst_ && desc_->cleanup) desc_->cleanup(inst_);
    inst_ = nullptr;
  }

  // Audio / CV buffers (instMax_ frames) connected at offset 0.
  bool allocate() {
    if (!inst_) return false;
    const size_t frames = static_cast<size_t>(std::max(instMax_, 1));
    audio_.resize(d_->ports.size());
    for (const Port& p : d_->ports) {
      if (p.type == Port::Audio || p.type == Port::CV) {
        audio_[p.index].assign(frames, 0.0f);
        desc_->connect_port(inst_, p.index, audio_[p.index].data());
      } else if (p.type == Port::Other) {
        // Unknown port types (old event ports…): a silent scratch buffer.
        if (atomBuf_[p.index].empty()) atomBuf_[p.index].assign(kAtomCapacity / 8, 0);
        desc_->connect_port(inst_, p.index, p.optional ? nullptr : atomBuf_[p.index].data());
      }
    }
    connectedOffset_ = 0;
    return true;
  }

  void activate() {
    if (active_ || !inst_) return;
    if (desc_->activate) desc_->activate(inst_);
    active_ = true;
    firstBlock_ = true;
  }
  void deactivate() {
    if (!active_) return;
    if (desc_->deactivate) desc_->deactivate(inst_);
    active_ = false;
  }

  // Latency is a control output, known after a run: one silent block, then
  // a fresh activation so the plugin starts clean.
  void measureLatency() {
    if (latencyPort_ < 0 || !active_) return;
    Transport t;
    t.sampleRate = sampleRate_;
    const int n = std::min(maxBlock_, 256);
    for (auto& b : audio_) std::fill(b.begin(), b.end(), 0.0f);
    const bool off = offline_.load();
    if (worker_) worker_->setSync(true);
    process(n, t);
    if (worker_) worker_->setSync(off);
    midiOut_.clear();
    dspToUi_.clear();
    latency_.store(std::max(0, static_cast<int>(std::lround(ctrl_[static_cast<size_t>(latencyPort_)]))));
    restartFlags_.fetch_and(~kRestartLatency);
    deactivate();
    activate();
    for (const auto& g : outGroups_)
      for (int p : g) std::fill(audio_[static_cast<size_t>(p)].begin(), audio_[static_cast<size_t>(p)].end(), 0.0f);
  }

  void closeEditorForReinstantiate() {
    if (onGoingAway) onGoingAway();
    if (editor_) editor_->shutdown();
  }

  // Port values and state properties (preset, saved state, default state).
  void applyState(const lv2::StateData& sd) {
    for (const auto& [sym, v] : sd.ports) {
      const int i = portIndex(sym.c_str());
      if (i < 0 || !ctl(static_cast<uint32_t>(i))) continue;
      ctrl_[static_cast<size_t>(i)] = v;
      cache_[static_cast<size_t>(i)].store(v, std::memory_order_relaxed);
    }
    if (!sd.props.empty() && stateIface_ && stateIface_->restore && inst_) {
      struct Item {
        LV2_URID key, type;
        std::string body;
      };
      std::vector<Item> items;
      for (const auto& p : sd.props) {
        Item it{U(p.key.c_str()), U(p.type.c_str()), p.data};
        if (it.type == uris().atomUrid) {  // stored as a URI
          LV2_URID id = U(p.data.c_str());
          it.body.assign(reinterpret_cast<const char*>(&id), sizeof id);
        }
        items.push_back(std::move(it));
      }
      LV2_Feature fWork{LV2_WORKER__schedule, worker_ ? worker_->syncSchedule() : nullptr};
      std::vector<const LV2_Feature*> feats = {&fMapPath_, &fFreePath_, &fMakePath_};
      if (worker_) feats.push_back(&fWork);
      feats.push_back(nullptr);
      stateIface_->restore(
          inst_,
          [](LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags) -> const void* {
            auto* v = static_cast<std::vector<Item>*>(h);
            for (const auto& it : *v)
              if (it.key == key) {
                if (size) *size = it.body.size();
                if (type) *type = it.type;
                if (flags) *flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
                return it.body.data();
              }
            return nullptr;
          },
          &items, 0, feats.data());
      // Worker responses of the restore (synchronous work) reach the plugin with the next block.
    }
    resendAll = true;
    dirty_.store(false);
  }

  void applyChange(const Change& c) {
    ctrl_[c.port] = c.value;
    cache_[c.port].store(c.value, std::memory_order_relaxed);
  }

  void connectAt(int offset) {
    if (offset == connectedOffset_) return;
    for (const Port& p : d_->ports)
      if (p.type == Port::Audio || p.type == Port::CV) desc_->connect_port(inst_, p.index, audio_[p.index].data() + offset);
    connectedOffset_ = offset;
  }

  void writePosition(const Transport& t) {
    const Uris& u = uris();
    lv2_atom_forge_frame_time(&forge_, 0);
    LV2_Atom_Forge_Frame f;
    lv2_atom_forge_object(&forge_, &f, 0, u.timePosition);
    const double unit = std::max(1, t.tsDen) / 4.0;  // beats per quarter note
    lv2_atom_forge_key(&forge_, u.timeFrame);
    lv2_atom_forge_long(&forge_, t.projectSamples);
    lv2_atom_forge_key(&forge_, u.timeSpeed);
    lv2_atom_forge_float(&forge_, t.playing ? 1.0f : 0.0f);
    lv2_atom_forge_key(&forge_, u.timeBpm);
    lv2_atom_forge_float(&forge_, static_cast<float>(t.tempo));
    lv2_atom_forge_key(&forge_, u.timeBar);
    lv2_atom_forge_long(&forge_, t.barNumber);
    lv2_atom_forge_key(&forge_, u.timeBarBeat);
    lv2_atom_forge_float(&forge_, static_cast<float>((t.projectQn - t.barQn) * unit));
    lv2_atom_forge_key(&forge_, u.timeBeat);
    lv2_atom_forge_double(&forge_, t.projectQn * unit);
    lv2_atom_forge_key(&forge_, u.timeBeatUnit);
    lv2_atom_forge_int(&forge_, std::max(1, t.tsDen));
    lv2_atom_forge_key(&forge_, u.timeBeatsPerBar);
    lv2_atom_forge_float(&forge_, static_cast<float>(std::max(1, t.tsNum)));
    lv2_atom_forge_key(&forge_, u.timeFps);
    lv2_atom_forge_float(&forge_, static_cast<float>(t.sampleRate));
    lv2_atom_forge_pop(&forge_, &f);
  }

  void runSegment(int off, int len, bool first, bool last, bool posChanged, const Transport& t) {
    const Uris& u = uris();
    connectAt(off);
    for (uint32_t i : atomIn_) {
      auto& buf = atomBuf_[i];
      lv2_atom_forge_set_buffer(&forge_, reinterpret_cast<uint8_t*>(buf.data()), buf.size() * 8);
      LV2_Atom_Forge_Frame seq;
      lv2_atom_forge_sequence_head(&forge_, &seq, 0);
      if (first) {
        for (int k = 0; k < uiMsgCount_; ++k) {
          const UiMsg& m = uiMsgs_[k];
          if (m.port != i || m.size < sizeof(LV2_Atom)) continue;
          lv2_atom_forge_frame_time(&forge_, 0);
          lv2_atom_forge_write(&forge_, uiMsg_.data() + m.at, m.size);
        }
        if (posChanged && port(i).timePosition) writePosition(t);
      }
      if (static_cast<int>(i) == midiIn_)
        for (int k = 0; k < eventCount_; ++k) {
          const MidiEv& e = events2_[k];
          if (e.offset < off || (e.offset >= off + len && !last)) continue;
          lv2_atom_forge_frame_time(&forge_, std::min(e.offset, off + len - 1) - off);
          lv2_atom_forge_atom(&forge_, e.size, u.midiEvent);
          lv2_atom_forge_write(&forge_, e.data, e.size);
        }
      lv2_atom_forge_pop(&forge_, &seq);
    }
    for (uint32_t i : atomOut_) {
      auto* a = reinterpret_cast<LV2_Atom*>(atomBuf_[i].data());
      a->size = static_cast<uint32_t>(atomBuf_[i].size() * 8 - sizeof(LV2_Atom));
      a->type = u.atomChunk;
    }
    desc_->run(inst_, static_cast<uint32_t>(len));
    for (uint32_t i : atomOut_) {
      const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(atomBuf_[i].data());
      if (seq->atom.type != u.atomSequence) continue;
      LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
        const uint8_t* body = static_cast<const uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
        if (ev->body.type == u.midiEvent && ev->body.size >= 1 && ev->body.size <= 3 && body[0] >= 0x80 && body[0] < 0xF0) {
          midiOut_.push(off + static_cast<int>(ev->time.frames), body[0], ev->body.size > 1 ? body[1] : 0, ev->body.size > 2 ? body[2] : 0);
        } else if ((ev->body.type == u.atomObject || ev->body.type == u.atomBlank) && ev->body.size >= sizeof(LV2_Atom_Object_Body)) {
          const auto* ob = reinterpret_cast<const LV2_Atom_Object_Body*>(body);
          if (ob->otype == u.stateChanged) dirty_.store(true);
        }
        if (uiActive_.load(std::memory_order_relaxed)) dspToUi_.write(&i, 4, &ev->body, static_cast<uint32_t>(sizeof(LV2_Atom) + ev->body.size));
      }
    }
    for (uint32_t i : ctlOut_) cache_[i].store(ctrl_[i], std::memory_order_relaxed);
    if (worker_) worker_->deliver();
  }

  std::shared_ptr<const lv2::PluginDesc> d_;
  std::shared_ptr<Dylib> lib_;
  const LV2_Descriptor* desc_ = nullptr;
  LV2_Handle inst_ = nullptr;
  const LV2_State_Interface* stateIface_ = nullptr;
  std::unique_ptr<Worker> worker_;
  std::unique_ptr<LV2_Worker_Schedule> workerSlot_;
  bool instanceAccess_ = false;
  std::vector<LV2_Options_Option> options_;
  float optRate_ = 48000;
  int32_t minBlock_ = 1, instMax_ = 4096, seqSize_ = 0;
  double instRate_ = 0, rangesRate_ = 0;
  std::vector<LV2_Feature> feats_;
  std::vector<const LV2_Feature*> featPtrs_;
  LV2_Feature fMapPath_{}, fFreePath_{}, fMakePath_{};
  LV2_Atom_Forge forge_{};

  std::vector<ParamInfo> params_;
  std::vector<std::unique_ptr<Ctl>> ctls_;  // per port (control inputs)
  std::vector<uint32_t> ctlIn_, ctlOut_, atomIn_, atomOut_;
  std::vector<uint32_t> mainIn_, scIn_;
  std::vector<std::vector<int>> outGroups_;
  int enabledPort_ = -1, freewheelPort_ = -1, bpmPort_ = -1, speedPort_ = -1, latencyPort_ = -1, midiIn_ = -1, midiOutPorts_ = 0;
  bool timePorts_ = false;
  std::vector<float> ctrl_;                         // control port buffers (connected)
  std::unique_ptr<std::atomic<float>[]> cache_;     // per port: last known value (any thread)
  std::vector<std::vector<float>> audio_;           // audio / CV port buffers
  std::vector<std::vector<uint64_t>> atomBuf_;      // atom port buffers (8-byte aligned)
  int connectedOffset_ = 0;

  double sampleRate_ = 0;
  int maxBlock_ = 0;
  bool active_ = false;
  std::atomic<int> latency_{0};
  std::atomic<bool> offline_{false}, bypass_{false};

  // audio thread
  std::unique_ptr<MidiEv[]> events2_;
  int eventCount_ = 0;
  std::unique_ptr<Change[]> changes_;
  int changeCount_ = 0;
  MidiOutBuffer midiOut_;
  bool firstBlock_ = true, lastPlaying_ = false;
  double lastTempo_ = 0;
  int lastNum_ = 0, lastDen_ = 0;
  int64_t expectFrame_ = 0;
  std::vector<uint8_t> uiMsg_;
  std::unique_ptr<UiMsg[]> uiMsgs_;
  size_t uiMsgBytes_ = 0;
  int uiMsgCount_ = 0;

  // host events / interface
  HostEventQueue events_;
  std::atomic<int32_t> restartFlags_{0};
  std::atomic<bool> dirty_{false}, uiActive_{false};
  Ring uiToDsp_, dspToUi_;
  Lv2Editor* editor_ = nullptr;
};

// ---------------------------------------------------------------- editor

std::unique_ptr<Editor> Lv2Backend::createEditor() {
  const int k = pickUi();
  if (k < 0 || editor_ || !inst_) return nullptr;
  const lv2::Ui& ui = d_->uis[static_cast<size_t>(k)];
  std::string err;
  auto lib = Dylib::open(ui.binary, &err);
  if (!lib) {
    std::fprintf(stderr, "[lv2] interface binary: %s\n", err.c_str());
    return nullptr;
  }
  using UiFn = const LV2UI_Descriptor* (*)(uint32_t);
  auto fn = toFunction<UiFn>(lib->function("lv2ui_descriptor"));
  const LV2UI_Descriptor* found = nullptr;
  for (uint32_t i = 0; fn && i < 1024; ++i) {
    const LV2UI_Descriptor* x = fn(i);
    if (!x) break;
    if (x->URI && ui.uri == x->URI) {
      found = x;
      break;
    }
  }
  if (!found) return nullptr;
  auto e = std::make_unique<Lv2Editor>(this, ui, lib, found);
  e->lastSent.assign(d_->ports.size(), std::nanf(""));
  uiStarted(e.get());
  return e;
}

Lv2Editor::~Lv2Editor() { shutdown(); }

void Lv2Editor::shutdown() {
  if (h_) {
    if (ui_.kind == lv2::Ui::External && widget_) {
      auto* w = static_cast<GridlineKxExternalWidget*>(widget_);
      if (w->hide && !closed_) w->hide(w);
    } else if (widget_) {
      lv2::native::remove(widget_);
    }
    if (d_->cleanup) d_->cleanup(h_);
    h_ = nullptr;
    widget_ = nullptr;
  }
  closed_ = true;
  if (b_) b_->editorGone(this);
  b_ = nullptr;
}

bool Lv2Editor::size(int& w, int& h) {
  if (!h_ || ui_.kind == lv2::Ui::External) return false;
  if (lv2::native::widgetSize(widget_, w, h)) return true;
  if (reqW_ > 0 && reqH_ > 0) {
    w = reqW_;
    h = reqH_;
    return true;
  }
  return false;
}

bool Lv2Editor::canResize() const {
  for (const auto* list : {&ui_.required, &ui_.optional})
    for (const auto& f : *list)
      if (f == LV2_UI__noUserResize || f == LV2_UI__fixedSize) return false;
  return ui_.kind != lv2::Ui::External && (resizeIface_ != nullptr || ui_.kind == lv2::Ui::Cocoa);
}

void Lv2Editor::setSize(int w, int h) {
  if (!h_) return;
  // As an interface of the UI, ui:resize takes the UI's own handle.
  if (resizeIface_ && resizeIface_->ui_resize) resizeIface_->ui_resize(h_, w, h);
  if (ui_.kind != lv2::Ui::External) lv2::native::setWidgetSize(widget_, w, h);
}

bool Lv2Editor::attach(void* parent) {
  if (!b_ || h_ || closed_) return false;
  const bool ext = ui_.kind == lv2::Ui::External;
  if (!ext && !parent) return false;
  const Uris& u = uris();
  rate_ = static_cast<float>(b_->sampleRate() > 0 ? b_->sampleRate() : 48000.0);
  options_ = {
      {LV2_OPTIONS_INSTANCE, 0, u.sampleRate, sizeof(float), u.atomFloat, &rate_},
      {LV2_OPTIONS_INSTANCE, 0, u.updateRate, sizeof(float), u.atomFloat, &updateRate_},
      {LV2_OPTIONS_INSTANCE, 0, u.scaleFactor, sizeof(float), u.atomFloat, &scale_},
      {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
  };
  hostResize_.handle = this;
  hostResize_.ui_resize = [](LV2UI_Feature_Handle h, int w, int hh) {
    auto* e = static_cast<Lv2Editor*>(h);
    e->reqW_ = w;
    e->reqH_ = hh;
    return e->requestResize && e->requestResize(w, hh) ? 0 : 1;
  };
  portMap_.handle = b_;
  portMap_.port_index = [](LV2UI_Feature_Handle h, const char* sym) {
    const int i = static_cast<Lv2Backend*>(h)->portIndex(sym);
    return i < 0 ? LV2UI_INVALID_PORT_INDEX : static_cast<uint32_t>(i);
  };
  touch_.handle = this;
  touch_.touch = [](LV2UI_Feature_Handle h, uint32_t p, bool grabbed) {
    auto* e = static_cast<Lv2Editor*>(h);
    e->touchSeen = true;
    if (e->b_) e->b_->uiTouch(p, grabbed);
  };
  dataAccess_.data_access = b_->descriptor()->extension_data;
  extHost_.ui_closed = [](LV2UI_Controller c) { static_cast<Lv2Editor*>(const_cast<void*>(c))->closed_ = true; };
  extHost_.plugin_human_id = b_->desc().name.c_str();
  feats_ = {
      {LV2_URID__map, &gMap},
      {LV2_URID__unmap, &gUnmap},
      {LV2_UI__resize, &hostResize_},
      {LV2_UI__portMap, &portMap_},
      {LV2_UI__touch, &touch_},
      {LV2_UI__idleInterface, nullptr},
      {LV2_INSTANCE_ACCESS_URI, b_->instance()},
      {LV2_DATA_ACCESS_URI, &dataAccess_},
      {LV2_OPTIONS__options, options_.data()},
      {LV2_LOG__log, &gLog},
  };
  if (ext) {
    feats_.push_back({lv2::kKxExternalHost, &extHost_});
    feats_.push_back({"http://lv2plug.in/ns/extensions/ui#external", &extHost_});
  } else {
    feats_.push_back({LV2_UI__parent, parent});
  }
  featPtrs_.clear();
  for (auto& f : feats_) featPtrs_.push_back(&f);
  featPtrs_.push_back(nullptr);
  h_ = d_->instantiate(
      d_, b_->desc().uri.c_str(), ui_.bundle.c_str(),
      [](LV2UI_Controller c, uint32_t portIdx, uint32_t size, uint32_t protocol, const void* buffer) {
        auto* e = static_cast<Lv2Editor*>(const_cast<void*>(c));
        if (e->b_) e->b_->uiWrite(e, portIdx, size, protocol, buffer);
      },
      this, &widget_, featPtrs_.data());
  if (!h_) return false;
  if (d_->extension_data) {
    idleIface_ = static_cast<const LV2UI_Idle_Interface*>(d_->extension_data(LV2_UI__idleInterface));
    resizeIface_ = static_cast<const LV2UI_Resize*>(d_->extension_data(LV2_UI__resize));
  }
  if (ext) {
    auto* w = static_cast<GridlineKxExternalWidget*>(widget_);
    if (!w || !w->show) return false;
    w->show(w);
  } else if (!lv2::native::embed(widget_, parent)) {
    return false;
  }
  b_->resendAll = true;
  idle();  // initial values
  return true;
}

void Lv2Editor::idle() {
  if (!h_ || !b_) return;
  if (ui_.kind == lv2::Ui::External && widget_ && !closed_) {
    auto* w = static_cast<GridlineKxExternalWidget*>(widget_);
    if (w->run) w->run(w);
  }
  if (closed_) return;
  if (idleIface_ && idleIface_->idle && idleIface_->idle(h_) != 0 && ui_.kind == lv2::Ui::External) closed_ = true;
  if (!d_->port_event) {
    b_->uiMessages(scratch_, [](uint32_t, const uint8_t*, uint32_t) {});
    return;
  }
  const bool all = b_->resendAll;
  b_->resendAll = false;
  const auto& ports = b_->desc().ports;
  for (const Port& p : ports) {
    if (p.type != Port::Control) continue;
    const float v = b_->portValue(p.index);
    if (!all && v == lastSent[p.index]) continue;
    lastSent[p.index] = v;
    d_->port_event(h_, p.index, sizeof(float), 0, &v);
  }
  const uint32_t transfer = uris().eventTransfer;
  b_->uiMessages(scratch_, [&](uint32_t portIdx, const uint8_t* atom, uint32_t size) { d_->port_event(h_, portIdx, size, transfer, atom); });
}

// ---------------------------------------------------------------- format

std::vector<std::string> lv2Paths() { return lv2::defaultPaths(); }

std::vector<std::string> lv2Discover(const std::vector<std::string>& folders) { return lv2::pluginBundles(folders); }

ScanResult lv2Scan(const std::string& bundle) {
  ScanResult r;
  r.format = "lv2";
  r.bundle = bundle;
  r.mtime = lv2::bundleMtime(bundle);
  std::string err;
  const std::vector<std::string> uris = lv2::pluginUris(bundle, &err);
  if (uris.empty()) {
    r.error = err.empty() ? "no plugins in the bundle" : err;
    return r;
  }
  for (const auto& uri : uris) {
    std::string perr;
    auto b = Lv2Backend::create(bundle, uri, &perr);
    if (!b) {
      r.error += (r.error.empty() ? "" : "; ") + uri + ": " + perr;
      continue;
    }
    const lv2::PluginDesc& d = b->desc();
    ScannedClass c;
    c.uid = uri;
    c.name = d.name;
    c.vendor = d.author;
    c.version = d.version;
    c.sdkVersion = "LV2";
    for (const auto& t : d.types) {
      const size_t cut = t.find('#');
      if (cut == std::string::npos || t.compare(0, cut + 1, lv2::kLv2) != 0) continue;
      const std::string cls = t.substr(cut + 1);
      if (cls != "Plugin") c.subCategories += (c.subCategories.empty() ? "" : "|") + cls;
    }
    c.eventInput = b->hasEventInput();
    c.eventOutput = b->hasEventOutput();
    if (b->prepare(48000.0, 512)) {
      c.inputChannels = b->mainInputChannels();
      c.outputChannels = b->mainOutputChannels();
      c.sidechainChannels = b->sidechainChannels();
      c.latency = b->latency();
      for (int k = 0; k < b->auxOutputCount(); ++k) c.auxOutputs.push_back(b->auxOutputName(k));
    }
    c.instrument = b->instrument();
    for (int k = 0; k < b->programCount(); ++k) c.programs.push_back(b->programName(k));
    for (const ParamInfo& p : b->params()) {
      ScannedParam sp;
      sp.id = p.id;
      sp.title = p.title;
      sp.units = p.units;
      sp.stepCount = p.stepCount;
      sp.def = p.def;
      sp.flags = p.flags;
      if (p.stepCount >= 1 && p.stepCount <= 32 && p.exposed())
        for (int k = 0; k <= p.stepCount; ++k) sp.labels.push_back(b->paramText(p.id, static_cast<double>(k) / p.stepCount));
      c.params.push_back(std::move(sp));
    }
    r.classes.push_back(std::move(c));
    (void)b.release();  // never destroyed in the scanner (like the other formats)
  }
  r.ok = !r.classes.empty();
  return r;
}

std::unique_ptr<Backend> lv2Create(const std::string& bundle, const std::string& uri, std::string* err) {
  return Lv2Backend::create(bundle, uri, err);
}

}  // namespace

Format lv2Format() {
  Format f{};
  f.id = "lv2";
  f.label = "LV2";
  f.defaultPaths = lv2Paths;
  f.discover = lv2Discover;
  f.scanInProcess = lv2Scan;
  f.create = lv2Create;
  f.mtime = lv2::bundleMtime;
  return f;
}

}  // namespace gl::host

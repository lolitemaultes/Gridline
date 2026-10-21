#include "Sandbox.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../PluginScan.h"

extern char** environ;

namespace gl::host {

namespace {

// ---------------------------------------------------------------- shared memory

constexpr uint32_t kShmMagic = 0x474C5342;  // "GLSB"
constexpr uint32_t kShmVersion = 2;
constexpr int kBlock = 1024;  // AudioEngine::kMaxBlock
constexpr int kOutBuses = 16;  // main + 15 extra outputs
constexpr int kMaxParams = 2048, kMaxEvents = 1024, kMaxMidiOut = 512;
constexpr int kRpcFd = 3, kWakeFd = 4;  // descriptors in the child

struct ParamChange {
  uint32_t id;
  int32_t offset;
  double value;
  int32_t mod;  // 1: modulation amount (addParamMod)
};

struct Shm {
  uint32_t magic, version;
  std::atomic<uint32_t> request, done;  // the audio handshake
  int32_t frames, ok;
  Transport transport;
  int32_t nParams, nEvents;
  ParamChange params[kMaxParams];
  NoteEvent events[kMaxEvents];
  float in[2][kBlock], sc[2][kBlock];
  float out[kOutBuses][2][kBlock];
  int32_t outCh[kOutBuses];
  int32_t nMidiOut;
  MidiOut midiOut[kMaxMidiOut];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics are required in shared memory");

// ---------------------------------------------------------------- messages

enum Op : uint8_t {
  OpCreate = 1,
  OpPrepare,
  OpRestart,
  OpGetState,
  OpSetState,
  OpGetValues,
  OpSetValue,
  OpParamText,
  OpIdle,
  OpSelectProgram,
  OpOpenEditor,
  OpCloseEditor,
  OpSetOffline,
  OpSetBypass,
  OpDiscoverPresets,
  OpLoadPreset,
};

struct Wr {
  std::string b;
  void u8(uint8_t v) { b += static_cast<char>(v); }
  void u32(uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }
  void i32(int32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }
  void f64(double v) { b.append(reinterpret_cast<const char*>(&v), 8); }
  void str(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    b += s;
  }
};

struct Rd {
  const std::string& b;
  size_t p = 0;
  bool ok = true;
  explicit Rd(const std::string& s) : b(s) {}
  bool need(size_t n) {
    if (p + n > b.size()) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? static_cast<uint8_t>(b[p++]) : 0; }
  uint32_t u32() {
    uint32_t v = 0;
    if (need(4)) std::memcpy(&v, b.data() + p, 4), p += 4;
    return v;
  }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  double f64() {
    double v = 0;
    if (need(8)) std::memcpy(&v, b.data() + p, 8), p += 8;
    return v;
  }
  std::string str() {
    uint32_t n = u32();
    if (!need(n)) return {};
    std::string s = b.substr(p, n);
    p += n;
    return s;
  }
};

bool writeAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Reads exactly n bytes; timeoutMs < 0 waits forever.
bool readAll(int fd, char* p, size_t n, int timeoutMs) {
  auto start = std::chrono::steady_clock::now();
  while (n > 0) {
    if (timeoutMs >= 0) {
      int left = timeoutMs - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
      if (left <= 0) return false;
      pollfd pf{fd, POLLIN, 0};
      int r = ::poll(&pf, 1, left);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) return false;
    }
    ssize_t got = ::read(fd, p, n);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
  }
  return true;
}

bool sendMsg(int fd, const std::string& payload) {
  uint32_t n = static_cast<uint32_t>(payload.size());
  return writeAll(fd, reinterpret_cast<const char*>(&n), 4) && writeAll(fd, payload.data(), payload.size());
}

bool recvMsg(int fd, std::string& payload, int timeoutMs) {
  uint32_t n = 0;
  if (!readAll(fd, reinterpret_cast<char*>(&n), 4, timeoutMs)) return false;
  if (n > (256u << 20)) return false;
  payload.resize(n);
  return n == 0 || readAll(fd, payload.data(), n, timeoutMs < 0 ? -1 : std::max(timeoutMs, 2000));
}

// Description of the child's instance (create / prepare / restart replies).
struct Desc {
  std::string format;
  std::vector<ParamInfo> params;
  bool eventInput = false, tuning = false, editor = false, eventOutput = false, bypass = false, notePan = false;
  int mainIn = 0, mainOut = 0, sc = 0;
  std::vector<std::string> auxNames;
  std::vector<int> outCh;
  std::vector<std::string> programs;
  int latency = 0, tail = 0;
  double sampleRate = 0;
  int maxBlock = 0;
  bool active = false;
};

void putDesc(Wr& w, Backend& b) {
  w.str(b.format());
  w.u32(static_cast<uint32_t>(b.params().size()));
  for (const auto& p : b.params()) {
    w.u32(p.id);
    w.str(p.title);
    w.str(p.units);
    w.i32(p.stepCount);
    w.f64(p.def);
    w.i32(p.flags);
  }
  w.u8(b.hasEventInput());
  w.u8(b.supportsTuning());
  w.u8(b.hasEditor());
  w.u8(b.hasEventOutput());
  w.u8(b.hasBypass());
  w.u8(b.handlesNotePan());
  w.i32(b.mainInputChannels());
  w.i32(b.mainOutputChannels());
  w.i32(b.sidechainChannels());
  const int aux = std::min(b.auxOutputCount(), kOutBuses - 1);
  w.i32(aux);
  for (int k = 0; k < aux; ++k) w.str(b.auxOutputName(k));
  for (int k = 0; k <= aux; ++k) w.i32(std::min(2, b.outputChannels(k)));
  w.i32(b.programCount());
  for (int k = 0; k < b.programCount(); ++k) w.str(b.programName(k));
  w.i32(b.latency());
  w.i32(b.tailSamples());
  w.f64(b.sampleRate());
  w.i32(b.maxBlock());
  w.u8(b.active());
}

bool getDesc(Rd& r, Desc& d) {
  d = Desc{};
  d.format = r.str();
  uint32_t n = r.u32();
  for (uint32_t i = 0; i < n && r.ok; ++i) {
    ParamInfo p;
    p.id = r.u32();
    p.title = r.str();
    p.units = r.str();
    p.stepCount = r.i32();
    p.def = r.f64();
    p.flags = r.i32();
    d.params.push_back(std::move(p));
  }
  d.eventInput = r.u8();
  d.tuning = r.u8();
  d.editor = r.u8();
  d.eventOutput = r.u8();
  d.bypass = r.u8();
  d.notePan = r.u8();
  d.mainIn = std::min(2, r.i32());
  d.mainOut = std::min(2, r.i32());
  d.sc = std::min(2, r.i32());
  int aux = r.i32();
  for (int k = 0; k < aux && r.ok; ++k) d.auxNames.push_back(r.str());
  for (int k = 0; k <= aux && r.ok; ++k) d.outCh.push_back(r.i32());
  int np = r.i32();
  for (int k = 0; k < np && r.ok; ++k) d.programs.push_back(r.str());
  d.latency = r.i32();
  d.tail = r.i32();
  d.sampleRate = r.f64();
  d.maxBlock = r.i32();
  d.active = r.u8();
  return r.ok;
}

// ---------------------------------------------------------------- parent side

class SandboxBackend;

class RemoteEditor final : public Editor {
 public:
  explicit RemoteEditor(SandboxBackend* b) : b_(b) {}
  ~RemoteEditor() override;
  bool size(int& w, int& h) override {
    w = 640;
    h = 420;
    return true;
  }
  bool attach(void* parent) override;
  bool external() const override { return true; }
  bool isOpen() const override;
  bool windowFrame(int& x, int& y, int& w, int& h) const override;
  bool popKey(KeyEvent& e) override;
  SandboxBackend* b_;
};

class SandboxBackend final : public Backend {
 public:
  static std::unique_ptr<SandboxBackend> create(const std::string& exe, const std::string& format, const std::string& entry,
                                                const std::string& uid, std::string* err, const std::string& name) {
    std::unique_ptr<SandboxBackend> s(new SandboxBackend);
    if (!s->spawn(exe, err)) return nullptr;
    Wr w;
    w.u8(OpCreate);
    w.str(format);
    w.str(entry);
    w.str(uid);
    w.str(name);
    std::string reply;
    if (!s->call(w.b, reply, 60000)) {
      if (err) *err = "the sandbox process did not start the plugin (" + s->crashReason() + ")";
      return nullptr;
    }
    Rd r(reply);
    if (!r.u8()) {
      if (err) *err = r.str();
      return nullptr;
    }
    if (!getDesc(r, s->desc_)) {
      if (err) *err = "bad reply from the sandbox process";
      return nullptr;
    }
    s->indexParams();
    s->label_ = s->desc_.format + " (sandbox)";
    return s;
  }

  ~SandboxBackend() override {
    if (editor_) editor_->b_ = nullptr;
    if (rpc_ >= 0) ::close(rpc_);
    if (wake_ >= 0) ::close(wake_);
    // The child exits on EOF; give it a moment, then make sure.
    for (int i = 0; i < 100 && !exited_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!exited_.load() && pid_ > 0) ::kill(pid_, SIGKILL);
    if (watcher_.joinable()) watcher_.join();
    if (shm_) {
      shm_->~Shm();
      ::munmap(shm_, sizeof(Shm));
    }
  }

  // ---- description ----
  const char* format() const override { return label_.c_str(); }
  const std::vector<ParamInfo>& params() const override { return desc_.params; }
  bool hasEventInput() const override { return desc_.eventInput; }
  int mainInputChannels() const override { return desc_.mainIn; }
  int mainOutputChannels() const override { return desc_.mainOut; }
  int sidechainChannels() const override { return desc_.sc; }
  int auxOutputCount() const override { return static_cast<int>(desc_.auxNames.size()); }
  std::string auxOutputName(int k) const override {
    return k >= 0 && static_cast<size_t>(k) < desc_.auxNames.size() ? desc_.auxNames[static_cast<size_t>(k)] : Backend::auxOutputName(k);
  }
  bool supportsTuning() const override { return desc_.tuning; }
  bool hasEventOutput() const override { return desc_.eventOutput; }
  bool supportsParamMod(uint32_t id) const override {
    const int i = paramIndex(id);
    return i >= 0 && (desc_.params[static_cast<size_t>(i)].flags & kParamModulatable) != 0;
  }
  bool hasBypass() const override { return desc_.bypass; }
  bool handlesNotePan() const override { return desc_.notePan; }
  bool setOffline(bool offline) override {
    Wr w;
    w.u8(OpSetOffline);
    w.u8(offline);
    std::string reply;
    return call(w.b, reply, 10000) && !reply.empty() && reply[0];
  }
  bool setBypass(bool on) override {
    Wr w;
    w.u8(OpSetBypass);
    w.u8(on);
    std::string reply;
    return call(w.b, reply, 10000) && !reply.empty() && reply[0];
  }
  std::vector<PresetRef> discoverPresets() override {
    std::vector<PresetRef> out;
    Wr w;
    w.u8(OpDiscoverPresets);
    std::string reply;
    if (!call(w.b, reply, 60000)) return out;
    Rd r(reply);
    const uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; ++i) {
      PresetRef p;
      p.name = r.str();
      p.category = r.str();
      p.creator = r.str();
      p.location = r.str();
      p.key = r.str();
      p.locationKind = r.u32();
      out.push_back(std::move(p));
    }
    return out;
  }
  bool loadPreset(const PresetRef& p) override {
    Wr w;
    w.u8(OpLoadPreset);
    w.str(p.location);
    w.str(p.key);
    w.u32(p.locationKind);
    std::string reply;
    values_.clear();
    texts_.clear();
    return call(w.b, reply, 20000) && !reply.empty() && reply[0];
  }

  // ---- UI thread ----
  bool prepare(double sr, int maxBlock) override {
    Wr w;
    w.u8(OpPrepare);
    w.f64(sr);
    w.i32(std::min(maxBlock, kBlock));
    return descCall(w, 30000);
  }
  double sampleRate() const override { return desc_.sampleRate; }
  int maxBlock() const override { return desc_.maxBlock; }
  bool active() const override { return desc_.active && !crashed_.load(); }
  void restart(int32_t flags) override {
    Wr w;
    w.u8(OpRestart);
    w.i32(flags);
    descCall(w, 30000);
    values_.clear();
  }
  int latency() const override { return desc_.latency; }
  int tailSamples() const override { return desc_.tail; }
  bool getState(std::string& data) override {
    Wr w;
    w.u8(OpGetState);
    std::string reply;
    if (!call(w.b, reply, 20000)) return false;
    Rd r(reply);
    if (!r.u8()) return false;
    data = r.str();
    return r.ok;
  }
  bool setState(const std::string& data) override {
    Wr w;
    w.u8(OpSetState);
    w.str(data);
    std::string reply;
    values_.clear();
    texts_.clear();
    return call(w.b, reply, 20000) && !reply.empty() && reply[0];
  }
  double paramNormalized(uint32_t id) const override {
    auto* self = const_cast<SandboxBackend*>(this);
    if (self->values_.empty()) self->fetchValues();
    int i = paramIndex(id);
    return i >= 0 && static_cast<size_t>(i) < values_.size() ? values_[static_cast<size_t>(i)] : 0.0;
  }
  void setParamNormalized(uint32_t id, double v) override {
    int i = paramIndex(id);
    if (i >= 0 && static_cast<size_t>(i) < values_.size()) values_[static_cast<size_t>(i)] = v;
    Wr w;
    w.u8(OpSetValue);
    w.u32(id);
    w.f64(v);
    std::string reply;
    call(w.b, reply, 5000);
  }
  std::string paramText(uint32_t id, double n) const override {
    auto* self = const_cast<SandboxBackend*>(this);
    auto key = std::make_pair(id, n);
    if (auto it = texts_.find(key); it != texts_.end()) return it->second;
    if (crashed_.load()) return {};
    Wr w;
    w.u8(OpParamText);
    w.u32(id);
    w.f64(n);
    std::string reply;
    if (!self->call(w.b, reply, 5000)) return {};
    Rd r(reply);
    std::string t = r.str();
    if (self->texts_.size() > 8192) self->texts_.clear();
    self->texts_[key] = t;
    return t;
  }
  void drainHostEvents(std::vector<HostEvent>& out) override {
    HostEvent e;
    while (pending_.pop(e)) out.push_back(e);
  }
  bool popOutputValue(OutputValue& v) override {
    if (outVals_.empty()) return false;
    v = outVals_.back();
    outVals_.pop_back();
    return true;
  }
  int programCount() const override { return static_cast<int>(desc_.programs.size()); }
  std::string programName(int i) const override {
    return i >= 0 && static_cast<size_t>(i) < desc_.programs.size() ? desc_.programs[static_cast<size_t>(i)] : std::string();
  }
  bool selectProgram(int i) override {
    Wr w;
    w.u8(OpSelectProgram);
    w.i32(i);
    std::string reply;
    values_.clear();
    texts_.clear();
    return call(w.b, reply, 20000) && !reply.empty() && reply[0];
  }
  bool hasEditor() const override {
#if defined(__APPLE__)
    return desc_.editor;
#else
    return false;  // editor windows of sandboxed plugins: macOS only for now
#endif
  }
  std::unique_ptr<Editor> createEditor() override {
    if (!hasEditor() || editor_ || crashed_.load()) return nullptr;
    auto e = std::make_unique<RemoteEditor>(this);
    editor_ = e.get();
    return e;
  }
  // Polls the child: its host events, processor output values, editor state.
  void idle() override {
    if (crashed_.load()) return;
    Wr w;
    w.u8(OpIdle);
    std::string reply;
    if (!call(w.b, reply, 5000)) return;
    Rd r(reply);
    editorOpen_ = r.u8();
    hasFrame_ = r.u8();
    for (int& v : frame_) v = r.i32();
    uint32_t n = r.u32();
    bool changed = false;
    for (uint32_t i = 0; i < n && r.ok; ++i) {
      HostEvent e;
      e.kind = static_cast<HostEvent::Kind>(r.u8());
      e.id = r.u32();
      e.value = r.f64();
      e.flags = r.i32();
      pending_.push(e);
      changed = true;
    }
    uint32_t nv = r.u32();
    for (uint32_t i = 0; i < nv && r.ok; ++i) {
      OutputValue v;
      v.id = r.u32();
      v.value = r.f64();
      outVals_.insert(outVals_.begin(), v);
      changed = true;
    }
    // Keys the interface did not use, typed in the sandbox window (J-66).
    uint32_t nk = r.u32();
    for (uint32_t i = 0; i < nk && r.ok && i < 256; ++i) {
      KeyEvent k;
      const uint8_t f = r.u8();
      k.down = (f & 1) != 0;
      k.repeat = (f & 2) != 0;
      k.code = static_cast<uint16_t>(r.u32());
      k.mods = r.u32();
      if (r.ok) keys_.push_back(k);
    }
    if (changed) {
      values_.clear();
      texts_.clear();
    }
  }
  bool crashed() const override { return crashed_.load(); }
  std::string crashReason() const override {
    std::lock_guard<std::mutex> g(reasonLock_);
    return reason_;
  }

  bool openEditor(int x, int y) {
    Wr w;
    w.u8(OpOpenEditor);
    w.i32(x);
    w.i32(y);
    std::string reply;
    editorOpen_ = call(w.b, reply, 20000) && !reply.empty() && reply[0];
    return editorOpen_;
  }
  void closeEditor() {
    editor_ = nullptr;
    if (!editorOpen_) return;
    editorOpen_ = false;
    Wr w;
    w.u8(OpCloseEditor);
    std::string reply;
    call(w.b, reply, 5000);
  }
  bool editorOpen() const { return editorOpen_ && !crashed_.load(); }
  bool editorFrame(int& x, int& y, int& w, int& h) const {
    if (!editorOpen() || !hasFrame_) return false;
    x = frame_[0], y = frame_[1], w = frame_[2], h = frame_[3];
    return true;
  }
  bool popKey(KeyEvent& k) {
    if (keys_.empty()) return false;
    k = keys_.front();
    keys_.erase(keys_.begin());
    return true;
  }
  std::vector<KeyEvent> keys_;
  RemoteEditor* editor_ = nullptr;
  bool hasFrame_ = false;
  int frame_[4] = {0, 0, 0, 0};

  // ---- audio thread ----
  float* inputChannel(int ch) override { return ch >= 0 && ch < desc_.mainIn && shm_ ? shm_->in[ch] : nullptr; }
  float* sidechainChannel(int ch) override { return ch >= 0 && ch < desc_.sc && shm_ ? shm_->sc[ch] : nullptr; }
  int outputChannels(int bus) const override {
    return bus >= 0 && static_cast<size_t>(bus) < desc_.outCh.size() ? desc_.outCh[static_cast<size_t>(bus)] : 0;
  }
  const float* outputChannel(int bus, int ch) override {
    if (ch < 0 || ch >= outputChannels(bus)) return nullptr;
    return silentOut_ ? silence_.data() : shm_->out[bus][ch];
  }
  bool addParamChange(uint32_t id, double v, int offset) override {
    if (nParams_ >= kMaxParams) return false;
    params_[nParams_++] = {id, offset, v, 0};
    return true;
  }
  bool addParamMod(uint32_t id, double amount, int offset) override {
    if (nParams_ >= kMaxParams || !supportsParamMod(id)) return false;
    params_[nParams_++] = {id, offset, amount, 1};
    return true;
  }
  bool popMidiOut(MidiOut& m) override {
    if (midiRead_ >= midiCount_) return false;
    m = midiBuf_[midiRead_++];
    return true;
  }
  void addEvent(const NoteEvent& e) override {
    if (nEvents_ < kMaxEvents) events_[nEvents_++] = e;
  }
  void clearInput() override {
    nParams_ = 0;
    nEvents_ = 0;
  }
  bool process(int n, const Transport& t) override {
    silentOut_ = true;
    midiCount_ = midiRead_ = 0;
    if (crashed_.load() || !shm_ || n <= 0 || n > kBlock) {
      clearInput();
      return false;
    }
    const auto start = std::chrono::steady_clock::now();
    // Budget: a few block lengths (the child runs the plugin in real time).
    const double budget = std::max(0.004, 4.0 * n / std::max(8000.0, t.sampleRate));
    if (inflight_) {  // the previous block came back late
      if (shm_->done.load(std::memory_order_acquire) != seq_) {
        lateSeconds_ += static_cast<double>(n) / std::max(8000.0, t.sampleRate);
        if (lateSeconds_ > 3.0) hang();
        return false;
      }
      inflight_ = false;
    }
    lateSeconds_ = 0;
    shm_->frames = n;
    shm_->transport = t;
    shm_->nParams = nParams_;
    std::memcpy(shm_->params, params_.get(), sizeof(ParamChange) * static_cast<size_t>(nParams_));
    shm_->nEvents = nEvents_;
    std::memcpy(static_cast<void*>(shm_->events), events_.get(), sizeof(NoteEvent) * static_cast<size_t>(nEvents_));
    nParams_ = nEvents_ = 0;
    const uint32_t seq = ++seq_;
    shm_->request.store(seq, std::memory_order_release);
    const char byte = 1;
    ssize_t wr = ::write(wake_, &byte, 1);
    (void)wr;
    for (uint32_t spin = 0;; ++spin) {
      if (shm_->done.load(std::memory_order_acquire) == seq) {
        silentOut_ = !shm_->ok;
        midiCount_ = std::clamp(shm_->nMidiOut, 0, kMaxMidiOut);
        std::memcpy(static_cast<void*>(midiBuf_.get()), shm_->midiOut, sizeof(MidiOut) * static_cast<size_t>(midiCount_));
        return shm_->ok != 0;
      }
      if (crashed_.load()) return false;
      if ((spin & 63) == 63) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budget) {
          inflight_ = true;  // keep waiting next block; output silence now
          return false;
        }
        std::this_thread::yield();
      }
    }
  }

 private:
  SandboxBackend() : silence_(kBlock, 0.0f), params_(new ParamChange[kMaxParams]), events_(new NoteEvent[kMaxEvents]), midiBuf_(new MidiOut[kMaxMidiOut]) {}

  bool spawn(const std::string& exe, std::string* err) {
    // Writing to a dead child's pipe / socket must fail with EPIPE, not kill us.
    static const bool sigpipeIgnored = [] {
      ::signal(SIGPIPE, SIG_IGN);
      return true;
    }();
    (void)sigpipeIgnored;
    static std::atomic<int> serial{0};
    shmName_ = "/glsb." + std::to_string(::getpid()) + "." + std::to_string(serial++);
    int fd = ::shm_open(shmName_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (err) *err = "cannot create shared memory";
      return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(sizeof(Shm))) != 0) {
      ::close(fd);
      ::shm_unlink(shmName_.c_str());
      if (err) *err = "cannot size shared memory";
      return false;
    }
    void* mem = ::mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) {
      ::shm_unlink(shmName_.c_str());
      if (err) *err = "cannot map shared memory";
      return false;
    }
    shm_ = new (mem) Shm;
    shm_->magic = kShmMagic;
    shm_->version = kShmVersion;
    shm_->request.store(0);
    shm_->done.store(0);

    int sv[2], pp[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || ::pipe(pp) != 0) {
      if (err) *err = "cannot create the sandbox channels";
      ::shm_unlink(shmName_.c_str());
      return false;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    ::fcntl(pp[1], F_SETFL, ::fcntl(pp[1], F_GETFL) | O_NONBLOCK);
    for (int f : {sv[0], pp[1]}) ::fcntl(f, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], kRpcFd);
    posix_spawn_file_actions_adddup2(&fa, pp[0], kWakeFd);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);  // only 0-4 reach the child
    posix_spawn_file_actions_addinherit_np(&fa, 0);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
#endif
    std::vector<std::string> args = {exe, "--plugin-sandbox", shmName_};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int rc = ::posix_spawn(&pid_, exe.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(sv[1]);
    ::close(pp[0]);
    rpc_ = sv[0];
    wake_ = pp[1];
    if (rc != 0) {
      ::shm_unlink(shmName_.c_str());
      pid_ = -1;
      exited_.store(true);
      if (err) *err = "cannot start the sandbox process";
      return false;
    }
    // The child opens the block first thing; unlink it once it has.
    watcher_ = std::thread([this]() {
      int status = 0;
      pid_t r;
      do {
        r = ::waitpid(pid_, &status, 0);
      } while (r < 0 && errno == EINTR);
      exited_.store(true);
      std::string why = r < 0 ? "lost the sandbox process"
                        : WIFSIGNALED(status) ? "the plugin crashed (signal " + std::to_string(WTERMSIG(status)) + ")"
                                              : "the sandbox process exited (code " + std::to_string(WEXITSTATUS(status)) + ")";
      markCrashed(why);
    });
    return true;
  }

  // One request/response on the socket (UI thread).
  bool call(const std::string& req, std::string& reply, int timeoutMs) {
    if (crashed_.load() || rpc_ < 0) return false;
    if (!sendMsg(rpc_, req) || !recvMsg(rpc_, reply, timeoutMs)) {
      if (!crashed_.load()) hang();
      return false;
    }
    if (!unlinked_) {
      ::shm_unlink(shmName_.c_str());
      unlinked_ = true;
    }
    return true;
  }
  bool descCall(const Wr& w, int timeoutMs) {
    std::string reply;
    if (!call(w.b, reply, timeoutMs)) return false;
    Rd r(reply);
    bool ok = r.u8();
    Desc d;
    if (getDesc(r, d)) {
      const bool paramsChanged = d.params.size() != desc_.params.size();
      desc_ = std::move(d);
      if (paramsChanged) indexParams();
    }
    values_.clear();
    return ok;
  }
  void fetchValues() {
    values_.assign(desc_.params.size(), 0.0);
    Wr w;
    w.u8(OpGetValues);
    std::string reply;
    if (!call(w.b, reply, 10000)) return;
    Rd r(reply);
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && i < values_.size() && r.ok; ++i) values_[i] = r.f64();
  }
  void markCrashed(const std::string& why) {
    {
      std::lock_guard<std::mutex> g(reasonLock_);
      if (reason_.empty()) reason_ = why;
    }
    crashed_.store(true);
  }
  // The child stopped answering: end it.
  void hang() {
    markCrashed("the plugin stopped responding");
    if (pid_ > 0 && !exited_.load()) ::kill(pid_, SIGKILL);
  }

  Desc desc_;
  std::string label_;
  std::string shmName_;
  bool unlinked_ = false;
  Shm* shm_ = nullptr;
  int rpc_ = -1, wake_ = -1;
  pid_t pid_ = -1;
  std::thread watcher_;
  std::atomic<bool> exited_{false}, crashed_{false};
  mutable std::mutex reasonLock_;
  std::string reason_;
  std::vector<double> values_;  // cached parameter values (UI)
  std::map<std::pair<uint32_t, double>, std::string> texts_;
  std::vector<OutputValue> outVals_;
  HostEventQueue pending_;
  bool editorOpen_ = false;
  // audio thread
  std::vector<float> silence_;
  bool silentOut_ = true;
  bool inflight_ = false;
  double lateSeconds_ = 0;
  uint32_t seq_ = 0;
  std::unique_ptr<ParamChange[]> params_;
  int nParams_ = 0;
  std::unique_ptr<NoteEvent[]> events_;
  int nEvents_ = 0;
  std::unique_ptr<MidiOut[]> midiBuf_;  // MIDI out of the last block
  int midiCount_ = 0, midiRead_ = 0;
};

RemoteEditor::~RemoteEditor() {
  if (b_) b_->closeEditor();
}
bool RemoteEditor::attach(void*) { return b_ && b_->openEditor(placeX, placeY); }
bool RemoteEditor::isOpen() const { return b_ && b_->editorOpen(); }
bool RemoteEditor::windowFrame(int& x, int& y, int& w, int& h) const { return b_ && b_->editorFrame(x, y, w, h); }
bool RemoteEditor::popKey(KeyEvent& e) { return b_ && b_->popKey(e); }

// ---------------------------------------------------------------- child side

struct Child {
  Shm* shm = nullptr;
  std::unique_ptr<Backend> backend;
  std::mutex lock;  // backend reconfiguration vs. processing
  std::unique_ptr<Editor> editor;
  std::string title;
  int idles = 0;  // idle calls since the editor opened

  void run() {
    std::lock_guard<std::mutex> g(lock);
    Backend* b = backend.get();
    const int n = shm->frames;
    if (!b || n <= 0 || n > kBlock || n > b->maxBlock()) {
      shm->ok = 0;
      shm->nMidiOut = 0;
      return;
    }
    for (int i = 0; i < std::min(shm->nParams, kMaxParams); ++i) {
      const ParamChange& c = shm->params[i];
      if (c.mod)
        b->addParamMod(c.id, c.value, c.offset);
      else
        b->addParamChange(c.id, c.value, c.offset);
    }
    for (int i = 0; i < std::min(shm->nEvents, kMaxEvents); ++i) b->addEvent(shm->events[i]);
    for (int c = 0; c < 2; ++c) {
      if (float* d = b->inputChannel(c)) std::memcpy(d, shm->in[c], sizeof(float) * static_cast<size_t>(n));
      if (float* d = b->sidechainChannel(c)) std::memcpy(d, shm->sc[c], sizeof(float) * static_cast<size_t>(n));
    }
    const bool ok = b->process(n, shm->transport);
    const int buses = std::min(kOutBuses, 1 + b->auxOutputCount());
    for (int k = 0; k < buses; ++k)
      for (int c = 0; c < 2; ++c) {
        const float* s = ok ? b->outputChannel(k, c < b->outputChannels(k) ? c : 0) : nullptr;
        if (s && b->outputChannels(k) > 0)
          std::memcpy(shm->out[k][c], s, sizeof(float) * static_cast<size_t>(n));
        else
          std::memset(shm->out[k][c], 0, sizeof(float) * static_cast<size_t>(n));
      }
    MidiOut m;
    int nm = 0;
    while (b->popMidiOut(m))
      if (nm < kMaxMidiOut) shm->midiOut[nm++] = m;
    shm->nMidiOut = nm;
    shm->ok = ok ? 1 : 0;
  }

  std::string handle(const std::string& msg) {
    Rd r(msg);
    Wr w;
    const uint8_t op = r.u8();
    switch (op) {
      case OpCreate: {
        std::string format = r.str(), entry = r.str(), uid = r.str(), name = r.str(), err;
        std::lock_guard<std::mutex> g(lock);
        backend = createBackend(format, entry, uid, &err);
        w.u8(backend != nullptr);
        if (!backend) {
          w.str(err.empty() ? "cannot load the plugin" : err);
          break;
        }
        title = name.empty() ? uid : name;
        putDesc(w, *backend);
        break;
      }
      case OpPrepare: {
        double sr = r.f64();
        int mb = r.i32();
        std::lock_guard<std::mutex> g(lock);
        w.u8(backend && backend->prepare(sr, mb));
        if (backend) putDesc(w, *backend);
        break;
      }
      case OpRestart: {
        int32_t flags = r.i32();
        std::lock_guard<std::mutex> g(lock);
        if (backend) backend->restart(flags);
        w.u8(backend != nullptr);
        if (backend) putDesc(w, *backend);
        break;
      }
      case OpGetState: {
        std::string data;
        bool ok = backend && backend->getState(data);
        w.u8(ok);
        w.str(data);
        break;
      }
      case OpSetState: {
        std::string data = r.str();
        std::lock_guard<std::mutex> g(lock);
        w.u8(backend && backend->setState(data));
        break;
      }
      case OpGetValues: {
        const auto& ps = backend ? backend->params() : std::vector<ParamInfo>();
        w.u32(static_cast<uint32_t>(ps.size()));
        for (const auto& p : ps) w.f64(backend->paramNormalized(p.id));
        break;
      }
      case OpSetValue: {
        uint32_t id = r.u32();
        double v = r.f64();
        if (backend) backend->setParamNormalized(id, v);
        w.u8(1);
        break;
      }
      case OpParamText: {
        uint32_t id = r.u32();
        double v = r.f64();
        w.str(backend ? backend->paramText(id, v) : std::string());
        break;
      }
      case OpIdle: {
        if (backend) backend->idle();
        if (editor) editor->idle();
        // Dev check (J-66): GRIDLINE_SANDBOX_DEV_KEYS types Space into the interface once.
        if (editor && ++idles == 40 && std::getenv("GRIDLINE_SANDBOX_DEV_KEYS")) sandboxDevKeys();
        if (editor) sandboxDevStats(idles);
        w.u8(editor && sandboxWindowOpen());
        {
          int fx = 0, fy = 0, fw = 0, fh = 0;
          const bool hasFrame = editor && sandboxWindowFrame(fx, fy, fw, fh);
          w.u8(hasFrame);
          for (int v : {fx, fy, fw, fh}) w.i32(v);
        }
        std::vector<HostEvent> evs;
        if (backend) backend->drainHostEvents(evs);
        w.u32(static_cast<uint32_t>(evs.size()));
        for (const auto& e : evs) {
          w.u8(static_cast<uint8_t>(e.kind));
          w.u32(e.id);
          w.f64(e.value);
          w.i32(e.flags);
        }
        std::vector<OutputValue> vals;
        OutputValue v;
        while (backend && backend->popOutputValue(v) && vals.size() < 4096) vals.push_back(v);
        w.u32(static_cast<uint32_t>(vals.size()));
        for (const auto& x : vals) {
          w.u32(x.id);
          w.f64(x.value);
        }
        std::vector<KeyEvent> keys;
        KeyEvent k;
        while (keys.size() < 256 && sandboxPopKey(k)) keys.push_back(k);
        w.u32(static_cast<uint32_t>(keys.size()));
        for (const auto& x : keys) {
          w.u8(static_cast<uint8_t>((x.down ? 1 : 0) | (x.repeat ? 2 : 0)));
          w.u32(x.code);
          w.u32(x.mods);
        }
        break;
      }
      case OpSelectProgram: {
        int i = r.i32();
        std::lock_guard<std::mutex> g(lock);
        w.u8(backend && backend->selectProgram(i));
        break;
      }
      case OpOpenEditor: {
        bool ok = false;
        const int px = r.i32(), py = r.i32();
        if (backend && !editor) {
          editor = backend->createEditor();
          int ew = 640, eh = 420;
          if (editor && editor->size(ew, eh)) {
            void* parent = sandboxOpenWindow(title + "  \xC2\xB7  Gridline sandbox", ew, eh, editor->canResize(), px, py);
            editor->requestResize = [](int nw, int nh) {
              sandboxResizeWindow(nw, nh);
              return true;
            };
            ok = parent && editor->attach(parent);
            idles = 0;
          }
          if (!ok) {
            editor.reset();
            sandboxCloseWindow();
          }
        }
        w.u8(ok);
        break;
      }
      case OpSetOffline: {
        const bool off = r.u8();
        std::lock_guard<std::mutex> g(lock);
        w.u8(backend && backend->setOffline(off));
        break;
      }
      case OpSetBypass: {
        const bool on = r.u8();
        w.u8(backend && backend->setBypass(on));
        break;
      }
      case OpDiscoverPresets: {
        std::vector<PresetRef> ps = backend ? backend->discoverPresets() : std::vector<PresetRef>();
        w.u32(static_cast<uint32_t>(ps.size()));
        for (const auto& p : ps) {
          w.str(p.name);
          w.str(p.category);
          w.str(p.creator);
          w.str(p.location);
          w.str(p.key);
          w.u32(p.locationKind);
        }
        break;
      }
      case OpLoadPreset: {
        PresetRef p;
        p.location = r.str();
        p.key = r.str();
        p.locationKind = r.u32();
        std::lock_guard<std::mutex> g(lock);
        w.u8(backend && backend->loadPreset(p));
        break;
      }
      case OpCloseEditor:
        editor.reset();
        sandboxCloseWindow();
        w.u8(1);
        break;
      default:
        w.u8(0);
        break;
    }
    return w.b;
  }
};

}  // namespace

std::unique_ptr<Backend> createSandboxBackend(const std::string& exe, const std::string& format, const std::string& entry,
                                              const std::string& uid, std::string* err, const std::string& name) {
  if (exe.empty()) {
    if (err) *err = "no executable for the sandbox process";
    return nullptr;
  }
  return SandboxBackend::create(exe, format, entry, uid, err, name.empty() ? uid : name);
}

int sandboxChildMain(const std::string& shmName) {
  ::signal(SIGPIPE, SIG_IGN);
  int fd = ::shm_open(shmName.c_str(), O_RDWR, 0600);
  if (fd < 0) return 4;
  void* mem = ::mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mem == MAP_FAILED) return 4;
  auto* child = new Child;  // never destroyed: the process leaves with _exit
  child->shm = static_cast<Shm*>(mem);
  if (child->shm->magic != kShmMagic || child->shm->version != kShmVersion) return 5;
  std::thread audio([child]() {
    char buf[64];
    uint32_t last = 0;
    for (;;) {
      ssize_t got = ::read(kWakeFd, buf, sizeof buf);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) std::_Exit(0);  // parent gone
      const uint32_t seq = child->shm->request.load(std::memory_order_acquire);
      if (seq == last) continue;
      last = seq;
      child->run();
      child->shm->done.store(seq, std::memory_order_release);
    }
  });
  audio.detach();
  for (;;) {
    int r = 0;
    if (child->editor) {
      // The interface's window is open: wait inside the Cocoa run loop (its
      // timers and redraws run meanwhile, J-66), woken by the parent's calls.
      r = sandboxWaitEvents(kRpcFd, 0.05) ? 1 : 0;
    } else {
      pollfd pf{kRpcFd, POLLIN, 0};
      r = ::poll(&pf, 1, 50);
    }
    if (r > 0) {
      std::string msg;
      if (!recvMsg(kRpcFd, msg, 10000)) std::_Exit(0);
      if (!sendMsg(kRpcFd, child->handle(msg))) std::_Exit(0);
    }
    if (child->editor) {
      sandboxPumpEvents();
      if (child->backend) child->backend->idle();
      child->editor->idle();
      if (!sandboxWindowOpen()) child->editor.reset();
    }
  }
}

#if !defined(__APPLE__)
bool sandboxWaitEvents(int fd, double seconds) {
  pollfd pf{fd, POLLIN, 0};
  return ::poll(&pf, 1, static_cast<int>(seconds * 1000.0)) > 0;
}
bool sandboxPopKey(KeyEvent&) { return false; }
void sandboxDevKeys() {}
void sandboxDevStats(int) {}
void* sandboxOpenWindow(const std::string&, int, int, bool, int, int) { return nullptr; }
bool sandboxWindowFrame(int&, int&, int&, int&) { return false; }
void sandboxResizeWindow(int, int) {}
bool sandboxWindowOpen() { return false; }
void sandboxCloseWindow() {}
void sandboxPumpEvents() {}
void* offscreenEditorParent(int, int) { return nullptr; }
void closeOffscreenEditorParent() {}
void pumpOffscreenEvents(double) {}
#endif

}  // namespace gl::host

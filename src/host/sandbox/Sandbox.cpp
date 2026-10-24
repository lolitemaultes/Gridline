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
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "../PluginScan.h"
#include "../../core/Platform.h"

namespace gl::host {

namespace {

// ---------------------------------------------------------------- shared memory

constexpr uint32_t kShmMagic = 0x474C5342;  // "GLSB"
constexpr uint32_t kShmVersion = 2;
constexpr int kBlock = 1024;  // AudioEngine::kMaxBlock
constexpr int kOutBuses = 16;  // main + 15 extra outputs
constexpr int kMaxParams = 2048, kMaxEvents = 1024, kMaxMidiOut = 512;
#if !defined(_WIN32)
constexpr int kRpcFd = 3, kWakeFd = 4;  // descriptors in the child
#endif

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

// A byte stream between the processes. POSIX: one socket (r == w) or a pipe end;
// Windows: anonymous pipe handles (one pipe per direction).
struct Chan {
  std::intptr_t r = -1, w = -1;
  bool valid() const { return r != -1 || w != -1; }
};

#if defined(_WIN32)
HANDLE handleOf(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

bool writeAll(const Chan& c, const char* p, size_t n) {
  while (n > 0) {
    DWORD done = 0;
    if (!WriteFile(handleOf(c.w), p, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &done, nullptr) || done == 0) return false;
    p += done;
    n -= done;
  }
  return true;
}

// Bytes waiting in the pipe; -1 when the other end is gone.
long pipeAvailable(const Chan& c) {
  DWORD avail = 0;
  if (!PeekNamedPipe(handleOf(c.r), nullptr, 0, nullptr, &avail, nullptr)) return -1;
  return static_cast<long>(avail);
}

bool waitReadable(const Chan& c, int timeoutMs) {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    const long a = pipeAvailable(c);
    if (a != 0) return true;  // data, or a closed pipe (the read reports it)
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) return false;
    Sleep(1);
  }
}

// Reads exactly n bytes; timeoutMs < 0 waits forever.
bool readAll(const Chan& c, char* p, size_t n, int timeoutMs) {
  auto start = std::chrono::steady_clock::now();
  while (n > 0) {
    if (timeoutMs >= 0) {
      int left = timeoutMs - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
      if (left <= 0 || !waitReadable(c, left)) return false;
    }
    DWORD got = 0;
    if (!ReadFile(handleOf(c.r), p, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

void closeChan(Chan& c) {
  if (c.r != -1) CloseHandle(handleOf(c.r));
  if (c.w != -1 && c.w != c.r) CloseHandle(handleOf(c.w));
  c = Chan{};
}
#else
bool writeAll(const Chan& c, const char* p, size_t n) {
  const int fd = static_cast<int>(c.w);
  while (n > 0) {
    auto w = ::write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool waitReadable(const Chan& c, int timeoutMs) {
  pollfd pf{static_cast<int>(c.r), POLLIN, 0};
  return ::poll(&pf, 1, timeoutMs) > 0;
}

// Reads exactly n bytes; timeoutMs < 0 waits forever.
bool readAll(const Chan& c, char* p, size_t n, int timeoutMs) {
  const int fd = static_cast<int>(c.r);
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
    auto got = ::read(fd, p, n);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
  }
  return true;
}

void closeChan(Chan& c) {
  if (c.r >= 0) ::close(static_cast<int>(c.r));
  if (c.w >= 0 && c.w != c.r) ::close(static_cast<int>(c.w));
  c = Chan{};
}
#endif

bool sendMsg(const Chan& c, const std::string& payload) {
  uint32_t n = static_cast<uint32_t>(payload.size());
  return writeAll(c, reinterpret_cast<const char*>(&n), 4) && writeAll(c, payload.data(), payload.size());
}

bool recvMsg(const Chan& c, std::string& payload, int timeoutMs) {
  uint32_t n = 0;
  if (!readAll(c, reinterpret_cast<char*>(&n), 4, timeoutMs)) return false;
  if (n > (256u << 20)) return false;
  payload.resize(n);
  return n == 0 || readAll(c, payload.data(), n, timeoutMs < 0 ? -1 : std::max(timeoutMs, 2000));
}

// ---------------------------------------------------------------- shared block

// Creates (parent) or opens (child) the named audio block. POSIX: shm_open; the parent unlinks
// the name once the child has it. Windows: a named file mapping in the session namespace,
// gone when both processes closed it.
struct ShmMap {
  void* mem = nullptr;
#if defined(_WIN32)
  HANDLE mapping = nullptr;
#endif
  bool create(const std::string& name, size_t size, std::string* err) {
#if defined(_WIN32)
    std::wstring w(name.begin(), name.end());  // ASCII
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), w.c_str());
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
      if (mapping) CloseHandle(mapping);
      mapping = nullptr;
      if (err) *err = "cannot create shared memory";
      return false;
    }
    mem = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!mem) {
      if (err) *err = "cannot map shared memory";
      return false;
    }
    return true;
#else
    int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (err) *err = "cannot create shared memory";
      return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
      ::close(fd);
      ::shm_unlink(name.c_str());
      if (err) *err = "cannot size shared memory";
      return false;
    }
    void* m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
      ::shm_unlink(name.c_str());
      if (err) *err = "cannot map shared memory";
      return false;
    }
    mem = m;
    return true;
#endif
  }
  bool open(const std::string& name, size_t size) {
#if defined(_WIN32)
    std::wstring w(name.begin(), name.end());
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
    if (!mapping) return false;
    mem = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    return mem != nullptr;
#else
    int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) return false;
    void* m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return false;
    mem = m;
    return true;
#endif
  }
  static void unlinkName(const std::string& name) {
#if defined(_WIN32)
    (void)name;
#else
    ::shm_unlink(name.c_str());
#endif
  }
  void close(size_t size) {
#if defined(_WIN32)
    (void)size;
    if (mem) UnmapViewOfFile(mem);
    if (mapping) CloseHandle(mapping);
    mapping = nullptr;
#else
    if (mem) ::munmap(mem, size);
#endif
    mem = nullptr;
  }
};

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
    closeChan(rpc_);
    closeChan(wake_);
    // The child exits on EOF; give it a moment, then make sure.
    for (int i = 0; i < 100 && !exited_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!exited_.load()) proc_.kill();
    if (watcher_.joinable()) watcher_.join();
    if (shm_) {
      shm_->~Shm();
      map_.close(sizeof(Shm));
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
  void setRenderingOffline(bool offline) override { offline_.store(offline, std::memory_order_relaxed); }
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
  std::string diagnostics() const override {
    char b[320];
    std::snprintf(b, sizeof b, "sandbox: blocks sent %u, answered %u, budget missed %u, in flight %d, last wait %.2f ms of %.2f ms (%s), crashed %d",
                  stSent_.load(std::memory_order_relaxed), shm_ ? shm_->done.load(std::memory_order_relaxed) : 0u,
                  stMissed_.load(std::memory_order_relaxed), stInflight_.load(std::memory_order_relaxed) ? 1 : 0,
                  stWaitUs_.load(std::memory_order_relaxed) / 1000.0, stBudgetUs_.load(std::memory_order_relaxed) / 1000.0,
                  offline_.load(std::memory_order_relaxed) ? "offline" : "real time", crashed_.load() ? 1 : 0);
    return b;
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
    // Budget: a few block lengths (the child runs the plugin in real time). Offline (export)
    // nothing waits for the sound card: every block is waited for, however slow the machine.
    const double budget = offline_.load(std::memory_order_relaxed) ? 30.0 : std::max(0.004, 4.0 * n / std::max(8000.0, t.sampleRate));
    if (inflight_) {  // the previous block came back late
      if (shm_->done.load(std::memory_order_acquire) != seq_) {
        lateSeconds_ += static_cast<double>(n) / std::max(8000.0, t.sampleRate);
        if (lateSeconds_ > 3.0) hang();
        return false;
      }
      inflight_ = false;
      stInflight_.store(false, std::memory_order_relaxed);
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
    stSent_.store(seq, std::memory_order_relaxed);
    stBudgetUs_.store(static_cast<uint32_t>(budget * 1e6), std::memory_order_relaxed);
    shm_->request.store(seq, std::memory_order_release);
    const char byte = 1;
#if defined(_WIN32)
    DWORD wr = 0;
    WriteFile(handleOf(wake_.w), &byte, 1, &wr, nullptr);  // PIPE_NOWAIT: never blocks
#else
    auto wr = ::write(static_cast<int>(wake_.w), &byte, 1);  // non-blocking
#endif
    (void)wr;
    for (uint32_t spin = 0;; ++spin) {
      if (shm_->done.load(std::memory_order_acquire) == seq) {
        stWaitUs_.store(static_cast<uint32_t>(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count()),
                        std::memory_order_relaxed);
        stInflight_.store(false, std::memory_order_relaxed);
        silentOut_ = !shm_->ok;
        midiCount_ = std::clamp(shm_->nMidiOut, 0, kMaxMidiOut);
        std::memcpy(static_cast<void*>(midiBuf_.get()), shm_->midiOut, sizeof(MidiOut) * static_cast<size_t>(midiCount_));
        return shm_->ok != 0;
      }
      if (crashed_.load()) return false;
      if ((spin & 63) == 63) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budget) {
          inflight_ = true;  // keep waiting next block; output silence now
          stWaitUs_.store(static_cast<uint32_t>(budget * 1e6), std::memory_order_relaxed);
          stMissed_.fetch_add(1, std::memory_order_relaxed);
          stInflight_.store(true, std::memory_order_relaxed);
          return false;
        }
        std::this_thread::yield();
      }
    }
  }

 private:
  SandboxBackend() : silence_(kBlock, 0.0f), params_(new ParamChange[kMaxParams]), events_(new NoteEvent[kMaxEvents]), midiBuf_(new MidiOut[kMaxMidiOut]) {}

  bool spawn(const std::string& exe, std::string* err) {
    static std::atomic<int> serial{0};
#if defined(_WIN32)
    shmName_ = "Local\\glsb." + std::to_string(platform::processId()) + "." + std::to_string(serial++);
#else
    // Writing to a dead child's pipe / socket must fail with EPIPE, not kill us.
    static const bool sigpipeIgnored = [] {
      ::signal(SIGPIPE, SIG_IGN);
      return true;
    }();
    (void)sigpipeIgnored;
    shmName_ = "/glsb." + std::to_string(platform::processId()) + "." + std::to_string(serial++);
#endif
    if (!map_.create(shmName_, sizeof(Shm), err)) return false;
    shm_ = new (map_.mem) Shm;
    shm_->magic = kShmMagic;
    shm_->version = kShmVersion;
    shm_->request.store(0);
    shm_->done.store(0);

    std::vector<std::string> args = {exe, "--plugin-sandbox", shmName_};
    std::vector<std::intptr_t> inherit;
    Chan childRpc, childWake;
#if defined(_WIN32)
    // Three anonymous pipes (requests, replies, wake-ups); the child's ends are passed on its
    // command line with the block's name, and only they are inherited.
    HANDLE toChildR = nullptr, toChildW = nullptr, fromChildR = nullptr, fromChildW = nullptr, wakeR = nullptr, wakeW = nullptr;
    if (!CreatePipe(&toChildR, &toChildW, nullptr, 1 << 16) || !CreatePipe(&fromChildR, &fromChildW, nullptr, 1 << 16) ||
        !CreatePipe(&wakeR, &wakeW, nullptr, 4096)) {
      if (err) *err = "cannot create the sandbox channels";
      return false;
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;  // the audio thread's wake-up never blocks
    SetNamedPipeHandleState(wakeW, &mode, nullptr, nullptr);
    rpc_ = Chan{reinterpret_cast<std::intptr_t>(fromChildR), reinterpret_cast<std::intptr_t>(toChildW)};
    wake_ = Chan{-1, reinterpret_cast<std::intptr_t>(wakeW)};
    childRpc = Chan{reinterpret_cast<std::intptr_t>(toChildR), reinterpret_cast<std::intptr_t>(fromChildW)};
    childWake = Chan{reinterpret_cast<std::intptr_t>(wakeR), -1};
    inherit = {childRpc.r, childRpc.w, childWake.r};
    args[2] += "|" + std::to_string(childRpc.r) + "|" + std::to_string(childRpc.w) + "|" + std::to_string(childWake.r) + "|" +
               std::to_string(platform::processId());
    const unsigned flags = platform::Process::kInheritStderr;
#else
    int sv[2], pp[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || ::pipe(pp) != 0) {
      if (err) *err = "cannot create the sandbox channels";
      ShmMap::unlinkName(shmName_);
      return false;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    ::fcntl(pp[1], F_SETFL, ::fcntl(pp[1], F_GETFL) | O_NONBLOCK);
    for (int f : {sv[0], pp[1]}) ::fcntl(f, F_SETFD, FD_CLOEXEC);
    rpc_ = Chan{sv[0], sv[0]};
    wake_ = Chan{-1, pp[1]};
    childRpc = Chan{sv[1], sv[1]};
    childWake = Chan{pp[0], -1};
    inherit = {sv[1], pp[0]};  // become descriptors 3 (kRpcFd) and 4 (kWakeFd) in the child
    const unsigned flags = platform::Process::kInheritStdin | platform::Process::kInheritStderr;
#endif
    const bool started = proc_.start(exe, args, flags, inherit);
    closeChan(childRpc);
    closeChan(childWake);
    if (!started) {
      ShmMap::unlinkName(shmName_);
      exited_.store(true);
      if (err) *err = "cannot start the sandbox process";
      return false;
    }
    // The child opens the block first thing; unlink it once it has.
    watcher_ = std::thread([this]() {
      proc_.wait();
      exited_.store(true);
      const platform::ExitStatus& st = proc_.status();
      std::string why = st.crashed ? "the plugin " + st.describe() : "the sandbox process exited (code " + std::to_string(st.code) + ")";
      markCrashed(why);
    });
    return true;
  }

  // One request/response on the socket (UI thread).
  bool call(const std::string& req, std::string& reply, int timeoutMs) {
    if (crashed_.load() || !rpc_.valid()) return false;
    if (!sendMsg(rpc_, req) || !recvMsg(rpc_, reply, timeoutMs)) {
      if (!crashed_.load()) hang();
      return false;
    }
    if (!unlinked_) {
      ShmMap::unlinkName(shmName_);
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
    if (!exited_.load()) proc_.kill();
  }

  Desc desc_;
  std::string label_;
  std::string shmName_;
  bool unlinked_ = false;
  Shm* shm_ = nullptr;
  ShmMap map_;
  Chan rpc_, wake_;
  platform::Process proc_;
  std::thread watcher_;
  std::atomic<bool> exited_{false}, crashed_{false};
  std::atomic<bool> offline_{false};  // rendering offline: no real-time budget
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
  // audio thread -> diagnostics() (relaxed: counters for messages only)
  std::atomic<uint32_t> stSent_{0}, stMissed_{0}, stWaitUs_{0}, stBudgetUs_{0};
  std::atomic<bool> stInflight_{false};
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

int sandboxChildMain(const std::string& arg) {
  std::string shmName = arg;
#if defined(_WIN32)
  // "<block name>|<request pipe>|<reply pipe>|<wake pipe>|<parent pid>" (see spawn()).
  std::vector<std::string> parts;
  for (size_t a = 0;;) {
    const size_t b = arg.find('|', a);
    parts.push_back(arg.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  if (parts.size() < 4) return 4;
  shmName = parts[0];
  const Chan rpc{static_cast<std::intptr_t>(std::stoll(parts[1])), static_cast<std::intptr_t>(std::stoll(parts[2]))};
  const Chan wake{static_cast<std::intptr_t>(std::stoll(parts[3])), -1};
#else
  ::signal(SIGPIPE, SIG_IGN);
  const Chan rpc{kRpcFd, kRpcFd};
  const Chan wake{kWakeFd, -1};
#endif
  ShmMap map;
  if (!map.open(shmName, sizeof(Shm))) return 4;
  auto* child = new Child;  // never destroyed: the process leaves with _exit
  child->shm = static_cast<Shm*>(map.mem);
  if (child->shm->magic != kShmMagic || child->shm->version != kShmVersion) return 5;
  std::thread audio([child, wake]() {
    char buf[64];
    uint32_t last = 0;
    for (;;) {
#if defined(_WIN32)
      DWORD got = 0;
      if (!ReadFile(handleOf(wake.r), buf, sizeof buf, &got, nullptr) || got == 0) std::_Exit(0);  // parent gone
#else
      auto got = ::read(static_cast<int>(wake.r), buf, sizeof buf);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) std::_Exit(0);  // parent gone
#endif
      const uint32_t seq = child->shm->request.load(std::memory_order_acquire);
      if (seq == last) continue;
      last = seq;
      child->run();
      child->shm->done.store(seq, std::memory_order_release);
    }
  });
  audio.detach();
  for (;;) {
    bool ready = false;
#if !defined(_WIN32)
    if (child->editor) {
      // The interface's window is open: wait inside the Cocoa run loop (its
      // timers and redraws run meanwhile, J-66), woken by the parent's calls.
      ready = sandboxWaitEvents(static_cast<int>(rpc.r), 0.05);
    } else
#endif
    {
      ready = waitReadable(rpc, 50);
    }
    if (ready) {
      std::string msg;
      if (!recvMsg(rpc, msg, 10000)) std::_Exit(0);
      if (!sendMsg(rpc, child->handle(msg))) std::_Exit(0);
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
#if defined(_WIN32)
  (void)fd;
  Sleep(static_cast<DWORD>(seconds * 1000.0));
  return false;
#else
  pollfd pf{fd, POLLIN, 0};
  return ::poll(&pf, 1, static_cast<int>(seconds * 1000.0)) > 0;
#endif
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

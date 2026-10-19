#include "HostedPlugins.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../engine/AudioEngine.h"
#include "PluginPrefs.h"
#include "sandbox/Sandbox.h"

namespace gl::host {

// ---------------------------------------------------------------- classes

double HostedClass::toNormalized(size_t k, double v) const {
  if (k >= params.size()) return std::clamp(v, 0.0, 1.0);
  if (isChoice(k)) return std::clamp(v / params[k].stepCount, 0.0, 1.0);
  return std::clamp(v, 0.0, 1.0);
}

double HostedClass::fromNormalized(size_t k, double n) const {
  n = std::clamp(n, 0.0, 1.0);
  if (k >= params.size()) return n;
  if (isChoice(k)) return std::round(n * params[k].stepCount);
  if (params[k].stepCount == 1) return n >= 0.5 ? 1.0 : 0.0;
  return n;
}

const char* HostedClass::formatLabel() const {
  const Format* f = findFormat(format);
  return f ? f->label : "Plugin";
}

HostedClass makeHostedClass(const ScannedClass& c, const std::string& bundle, const std::string& format) {
  HostedClass h;
  h.type = format + ":" + c.uid;
  h.format = format;
  h.bundle = bundle;
  h.uid = c.uid;
  h.name = c.name;
  h.vendor = c.vendor;
  h.version = c.version;
  h.subCategories = c.subCategories;
  h.instrument = c.instrument;
  h.sidechain = c.sidechainChannels > 0;
  h.eventInput = c.eventInput;
  h.eventOutput = c.eventOutput;
  h.auxOutputs = c.auxOutputs;
  if (h.auxOutputs.size() > static_cast<size_t>(kMaxAuxOutputs)) h.auxOutputs.resize(kMaxAuxOutputs);
  h.programs = c.programs;
  const size_t cap = static_cast<size_t>(c.instrument ? kMaxInstrumentParams : kMaxEffectParams);
  // Plugins that flag automatable parameters get only those (wrappers add
  // thousands of non-automatable MIDI-controller proxies); otherwise every
  // writable, visible parameter.
  const bool anyAutomatable = std::any_of(c.params.begin(), c.params.end(), [](const ScannedParam& p) { return (p.flags & kParamAutomatable) != 0; });
  for (const auto& p : c.params) {
    if (p.flags & (kParamReadOnly | kParamHidden | kParamBypass)) continue;
    if (anyAutomatable && !(p.flags & kParamAutomatable)) continue;
    if (h.params.size() >= cap) break;
    h.params.push_back(p);
  }
  return h;
}

PluginInfo makePluginInfo(const HostedClass& c) {
  PluginInfo info;
  info.type = c.type;
  info.name = c.name;
  info.category = c.vendor.empty() ? std::string(c.formatLabel()) : c.vendor;
  info.kind = c.instrument ? PluginKind::Generator : PluginKind::Effect;
  for (size_t k = 0; k < c.params.size(); ++k) {
    const auto& p = c.params[k];
    ParamDesc d;
    d.name = p.title.empty() ? "Param " + std::to_string(p.id) : p.title;
    d.unit = p.units;
    d.min = 0;
    d.max = 1;
    if (c.isChoice(k)) {
      d.kind = ParamKind::Choice;
      d.choices = p.labels;
      d.max = p.stepCount;
      d.def = std::round(p.def * p.stepCount);
      d.unit.clear();
    } else if (p.stepCount == 1) {
      d.kind = ParamKind::Toggle;
      d.def = p.def >= 0.5 ? 1 : 0;
    } else {
      d.def = p.def;
    }
    info.params.push_back(std::move(d));
  }
  // Programs are the plugin's factory presets (selected through the instance,
  // not by parameter values; see the plugin header in the app).
  for (const auto& name : c.programs) {
    PluginInfo::Preset pr;
    pr.name = name;
    info.presets.push_back(std::move(pr));
  }
  return info;
}

namespace {
std::mutex gClassLock;
std::map<std::string, std::shared_ptr<const HostedClass>>& classMap() {
  static auto* m = new std::map<std::string, std::shared_ptr<const HostedClass>>;
  return *m;
}
std::mutex gErrLock;
std::string gLastError;
void setError(const std::string& e) {
  std::lock_guard<std::mutex> g(gErrLock);
  gLastError = e;
}
std::atomic<uint64_t> gGeneration{0};
std::mutex gExeLock;
std::string gChildExe;
}  // namespace

std::string lastError() {
  std::lock_guard<std::mutex> g(gErrLock);
  return gLastError;
}

void setChildExecutable(const std::string& exe) {
  std::lock_guard<std::mutex> g(gExeLock);
  gChildExe = exe;
}

std::string childExecutable() {
  std::lock_guard<std::mutex> g(gExeLock);
  return gChildExe.empty() ? selfExecutablePath() : gChildExe;
}

void registerScanResult(const ScanResult& r) {
  if (!r.ok) return;
  for (const auto& c : r.classes) {
    auto h = std::make_shared<const HostedClass>(makeHostedClass(c, r.bundle, r.format));
    registerDynamicPlugin(makePluginInfo(*h));
    std::lock_guard<std::mutex> g(gClassLock);
    classMap()[h->type] = h;
  }
  ++gGeneration;
}

std::shared_ptr<const HostedClass> hostedClass(const std::string& type) {
  std::lock_guard<std::mutex> g(gClassLock);
  auto it = classMap().find(type);
  return it == classMap().end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const HostedClass>> hostedClasses() {
  std::lock_guard<std::mutex> g(gClassLock);
  std::vector<std::shared_ptr<const HostedClass>> v;
  for (const auto& [k, c] : classMap()) v.push_back(c);
  return v;
}

// ---------------------------------------------------------------- state chunks

std::string encodeState(const std::string& component, const std::string& controller) {
  return "vst3s1:" + base64Encode(component) + ":" + base64Encode(controller);
}

bool decodeState(const std::string& data, std::string& component, std::string& controller) {
  component.clear();
  controller.clear();
  if (data.compare(0, 7, "vst3s1:") != 0) return false;
  size_t colon = data.find(':', 7);
  if (colon == std::string::npos) return false;
  return base64Decode(data.substr(7, colon - 7), component) && base64Decode(data.substr(colon + 1), controller);
}

// ---------------------------------------------------------------- adapter core

namespace {

int64_t monoNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void atomicMax(std::atomic<float>& a, float v) {
  float cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

// Shared by the instrument and effect adapters: parameter mapping, state
// loading, the audio-side parameter diff, event scheduling, the fixed size
// buffer and the UI pump.
//
// Scheduling: everything the engine sends (notes, MIDI, parameter values,
// modulation) is stamped with an absolute input sample position (inClock_ +
// offset) and handed to the plugin with the block that contains it, at its
// offset inside that block. Without fixed size buffers the plugin blocks
// are the engine's blocks (split at maxBlock); with them the plugin always
// processes exactly fixedSize frames through a FIFO (fixedSize frames of
// added latency, reported for delay compensation).
class Core final : public HostedInstance {
 public:
  static constexpr int kMaxPending = 4096;
  static constexpr int kMaxMidiOut = 1024;

  Core(std::shared_ptr<const HostedClass> cls, std::unique_ptr<Backend> b, bool sandboxed)
      : cls_(std::move(cls)), b_(std::move(b)), sandboxed_(sandboxed), opts_(hostOptions(cls_->type)) {
    const size_t n = cls_->params.size();
    ids_.resize(n);
    mapped_.resize(n);
    modulated_.resize(n);
    for (size_t k = 0; k < n; ++k) {
      ids_[k] = cls_->params[k].id;
      mapped_[k] = b_->paramIndex(ids_[k]) >= 0;
      modulated_[k] = opts_.modulation && mapped_[k] && b_->supportsParamMod(ids_[k]) && !cls_->isChoice(k) && cls_->params[k].stepCount == 0;
    }
    sent_.assign(n, std::nanf(""));
    sentMod_.assign(n, 0.0);
    ctrlKnown_.assign(n, std::nanf(""));
    audioVals_.reset(new std::atomic<float>[n == 0 ? 1 : n]);
    base_.reset(new std::atomic<float>[n == 0 ? 1 : n]);
    for (size_t k = 0; k < n; ++k) {
      audioVals_[k].store(std::nanf(""), std::memory_order_relaxed);
      base_[k].store(std::nanf(""), std::memory_order_relaxed);
    }
    pending_.reset(new Pending[kMaxPending]);
    midiOut_.reset(new Generator::MidiOutEvent[kMaxMidiOut]);
    creator_ = std::this_thread::get_id();
    fixedSize_ = opts_.fixedBuffers ? std::clamp(opts_.fixedSize, 32, AudioEngine::kMaxBlock) : 0;
    if (fixedSize_ > 0) {
      const int buses = 1 + std::min(std::max(b_->auxOutputCount(), static_cast<int>(cls_->auxOutputs.size())), kMaxAuxOutputs);
      fifoIn_.assign(static_cast<size_t>(4 * fixedSize_), 0.0f);  // main L/R, sidechain L/R
      fifoOut_.assign(static_cast<size_t>(buses * 2 * fixedSize_), 0.0f);
      fifoBuses_ = buses;
    }
  }
  ~Core() override {
    if (b_->onGoingAway) b_->onGoingAway();
    suspend();
  }

  Backend* backend() override { return b_.get(); }
  const HostedClass& hostedClass() const override { return *cls_; }
  bool sandboxed() const override { return sandboxed_; }
  bool crashed() const override { return b_->crashed(); }
  std::string crashReason() const override { return b_->crashReason(); }
  void requestReload() override { reload_.store(true); }
  const HostOptions& options() const override { return opts_; }
  int totalLatency() const override { return b_->latency() + fixedSize_; }
  bool stale() const {
    return reload_.load() || runsInSandbox(cls_->type) != sandboxed_ || !hostOptions(cls_->type).sameStructure(opts_);
  }
  void takeCpu(float& average, float& peak) override {
    const uint64_t ns = cpuNs_.exchange(0, std::memory_order_relaxed);
    const uint64_t frames = cpuFrames_.exchange(0, std::memory_order_relaxed);
    const double sr = b_->sampleRate() > 0 ? b_->sampleRate() : 48000.0;
    average = frames > 0 ? static_cast<float>(static_cast<double>(ns) / (static_cast<double>(frames) / sr * 1e9)) : 0.0f;
    peak = cpuPeak_.exchange(0, std::memory_order_relaxed);
  }
  uint64_t midiOutCount() const override { return midiOutTotal_.load(std::memory_order_relaxed); }
  bool hasMidiOutput() const override { return b_->hasEventOutput(); }

  // ---- UI thread ----
  void prepareHost(const PluginState& st, double sr) {
    for (size_t k = 0; k < ids_.size(); ++k) base_[k].store(k < st.params.size() ? st.params[k] : std::nanf(""), std::memory_order_relaxed);
    if (b_->crashed()) return;
    const bool needLoad = !loaded_ || st.data != lastData_;
    const bool needPrepare = !b_->active() || b_->sampleRate() != sr;
    if (!needLoad && !needPrepare) return;
    suspend();
    if (needPrepare) b_->prepare(sr, AudioEngine::kMaxBlock);
    if (needLoad) {
      if (!st.data.empty()) b_->setState(st.data);
      lastData_ = st.data;
      loaded_ = true;
      if (st.data.empty()) {
        // No plugin state yet (new instance, imported values): the song's
        // values are the truth — every one goes to the plugin with the next block.
        songValues_ = st.params;
        for (size_t k = 0; k < sent_.size(); ++k) {
          sent_[k] = std::nanf("");
          sentMod_[k] = 0.0;
          ctrlKnown_[k] = std::nanf("");
        }
        readbackPending_ = true;  // reports the song's own values (nothing changes)
        readbackFromSong_ = true;
        resetSchedule();
        resume();
        return;
      }
      // The song's values are what the processor has now (no resend); the
      // controller's values are read back into the song by pumpUi.
      for (size_t k = 0; k < sent_.size(); ++k) {
        sent_[k] = k < st.params.size() ? st.params[k] : std::nanf("");
        sentMod_[k] = 0.0;
        float c = mapped_[k] ? static_cast<float>(cls_->fromNormalized(k, b_->paramNormalized(ids_[k]))) : sent_[k];
        ctrlKnown_[k] = c;
        audioVals_[k].store(c, std::memory_order_relaxed);
      }
      readbackPending_ = true;
      readbackFromSong_ = false;
    }
    resetSchedule();
    resume();
  }

  void pumpUi(UiUpdate& u) override {
    if (b_->crashed()) {
      if (!crashReported_) u.crashed = true;
      crashReported_ = true;
      return;
    }
    if (const int off = offlineRequest_.exchange(-1); off >= 0 && off != offlineApplied_) {
      suspend();
      b_->setOffline(off == 1);
      offlineApplied_ = off;
      resume();
    }
    b_->idle();
    events_.clear();
    b_->drainHostEvents(events_);
    for (const auto& e : events_) {
      switch (e.kind) {
        case HostEvent::PerformEdit: {
          int k = indexOf(e.id);
          if (k < 0) break;
          float v = static_cast<float>(cls_->fromNormalized(static_cast<size_t>(k), e.value));
          ctrlKnown_[static_cast<size_t>(k)] = v;
          u.edits.push_back({k, v});
          break;
        }
        case HostEvent::EndEdit:
          u.gestureEnd = true;
          break;
        case HostEvent::Restart:
          if (e.flags & (kRestartReload | kRestartLatency | kRestartParamInfo)) {
            suspend();
            b_->restart(e.flags);
            resetSchedule();
            resume();
          }
          if (e.flags & (kRestartParamValues | kRestartParamInfo)) {
            readbackPending_ = true;
            readbackFromSong_ = false;
            u.captureState = true;
          }
          break;
        case HostEvent::Dirty:
          u.captureState = true;
          break;
        case HostEvent::BeginEdit:
          break;
      }
    }
    // Values the processor changed itself go to the controller.
    OutputValue ov{};
    while (b_->popOutputValue(ov)) {
      b_->setParamNormalized(ov.id, ov.value);
      int k = indexOf(ov.id);
      if (k >= 0) ctrlKnown_[static_cast<size_t>(k)] = static_cast<float>(cls_->fromNormalized(static_cast<size_t>(k), ov.value));
    }
    // Values the engine sent (automation, links, knob edits) go to the controller.
    for (size_t k = 0; k < ids_.size(); ++k) {
      float a = audioVals_[k].load(std::memory_order_relaxed);
      if (std::isnan(a) || a == ctrlKnown_[k] || !mapped_[k]) continue;
      b_->setParamNormalized(ids_[k], cls_->toNormalized(k, a));
      ctrlKnown_[k] = a;
    }
    if (programCapture_) {
      programCapture_ = false;
      u.captureState = true;
    }
    if (readbackPending_) {
      readbackPending_ = false;
      u.readback = true;
      u.values.resize(ids_.size());
      for (size_t k = 0; k < ids_.size(); ++k) {
        float c = readbackFromSong_ ? (k < songValues_.size() ? songValues_[k] : std::nanf(""))
                  : mapped_[k]      ? static_cast<float>(cls_->fromNormalized(k, b_->paramNormalized(ids_[k])))
                                    : sent_[k];
        u.values[k] = std::isnan(c) ? static_cast<float>(cls_->fromNormalized(k, cls_->params[k].def)) : c;
        if (!readbackFromSong_) ctrlKnown_[k] = u.values[k];
      }
      readbackFromSong_ = false;
    }
  }

  bool captureState(std::string& data) override {
    if (b_->crashed() || !b_->getState(data)) return false;
    lastData_ = data;
    return true;
  }

  std::string paramText(int index, float value) override {
    if (b_->crashed() || index < 0 || static_cast<size_t>(index) >= ids_.size() || !mapped_[static_cast<size_t>(index)]) return {};
    return b_->paramText(ids_[static_cast<size_t>(index)], cls_->toNormalized(static_cast<size_t>(index), value));
  }

  int programCount() const override { return b_->programCount(); }
  std::string programName(int i) const override { return b_->programName(i); }
  bool selectProgram(int i) override {
    if (b_->crashed()) return false;
    suspend();
    bool ok = b_->selectProgram(i);
    resume();
    if (ok) {
      readbackPending_ = true;
      readbackFromSong_ = false;
      programCapture_ = true;
    }
    return ok;
  }
  const std::vector<PresetRef>& libraryPresets() override {
    if (!presetsScanned_ && !b_->crashed()) {
      presets_ = b_->discoverPresets();
      presetsScanned_ = true;
    }
    return presets_;
  }
  bool loadLibraryPreset(size_t i) override {
    if (b_->crashed() || i >= libraryPresets().size()) return false;
    suspend();
    bool ok = b_->loadPreset(presets_[i]);
    resetSchedule();
    resume();
    if (ok) {
      readbackPending_ = true;
      readbackFromSong_ = false;
      programCapture_ = true;
    }
    return ok;
  }

  // ---- audio thread ----
  // Enter/leave the audio-side section; false while the UI reconfigures.
  bool enter() {
    busy_.store(true);
    if (suspended_.load()) {
      busy_.store(false);
      return false;
    }
    return true;
  }
  void leave() { busy_.store(false); }

  // Start of an engine block: its context and where it starts on the input clock.
  void beginBlock(const ProcessContext& ctx) {
    ctx_ = ctx;
    ctxClock_ = inClock_;
    const int want = ctx.offline && opts_.notifyRender ? 1 : 0;
    if (want != offlineApplied_) {
      // Offline exports run on the thread that created the instance: switch
      // right here, between two process calls; otherwise ask the UI thread.
      if (std::this_thread::get_id() == creator_) {
        b_->setOffline(want == 1);
        offlineApplied_ = want;
      } else {
        offlineRequest_.store(want);
      }
    }
  }

  // Changed parameter values (and modulation amounts) at the current input position.
  void sendParams(const PluginState& st) {
    const size_t n = std::min(sent_.size(), st.params.size());
    for (size_t k = 0; k < n; ++k) {
      if (!mapped_[k]) continue;
      float v = st.params[k];
      if (modulated_[k]) {
        // The plugin keeps the song's value; the difference is modulation.
        const float base = base_[k].load(std::memory_order_relaxed);
        const float b = std::isnan(base) ? v : base;
        if (b != sent_[k]) {
          if (!queueParam(ids_[k], cls_->toNormalized(k, b), 0, false)) break;
          sent_[k] = b;
          audioVals_[k].store(b, std::memory_order_relaxed);
        }
        const double mod = cls_->toNormalized(k, v) - cls_->toNormalized(k, b);
        if (std::fabs(mod - sentMod_[k]) > 1e-9) {
          if (!queueParam(ids_[k], mod, 0, true)) break;
          sentMod_[k] = mod;
        }
        continue;
      }
      if (v == sent_[k]) continue;
      if (!queueParam(ids_[k], cls_->toNormalized(k, v), 0, false)) break;  // full: the rest go next block
      sent_[k] = v;
      audioVals_[k].store(v, std::memory_order_relaxed);
    }
  }

  // A note / MIDI event `offset` samples after the current input position.
  void queueEvent(const NoteEvent& e, int offset) {
    if (pendingCount_ >= kMaxPending) return;
    Pending p;
    p.at = inClock_ + std::max(0, offset);
    p.kind = Pending::Event;
    p.ev = e;
    insert(p);
  }
  void dropInput() {
    pendingCount_ = 0;
    b_->clearInput();
  }

  // Runs n engine frames. in: effect input (nullptr = silence); sc: sidechain;
  // out: overwritten (effects) or added to (instruments); aux: extra outputs
  // [k][L, R][kMaxBlock] written at auxPos.
  void run(int n, const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, bool add, float* aux,
           int auxCount, int auxPos) {
    Backend* p = b_.get();
    if (fixedSize_ <= 0) {
      const int maxb = std::max(1, p->maxBlock());
      for (int done = 0; done < n;) {
        int len = std::min(n - done, maxb);
        if (!opts_.maxBuffers)  // split the block at the next note / MIDI event
          for (int q = 0; q < pendingCount_; ++q) {
            const int64_t at = pending_[q].at - (inClock_ + done);
            if (pending_[q].kind == Pending::Event && at > 0) {
              len = static_cast<int>(std::min<int64_t>(len, at));
              break;
            }
          }
        feed(inL ? inL + done : nullptr, inR ? inR + done : nullptr, scL ? scL + done : nullptr, scR ? scR + done : nullptr, len);
        const bool ok = processBlock(inClock_ + done, len);
        for (int bus = 0; bus <= auxCount; ++bus) {
          float* dl = bus == 0 ? outL + done : aux + static_cast<size_t>(bus - 1) * 2 * AudioEngine::kMaxBlock + auxPos + done;
          float* dr = bus == 0 ? outR + done : dl + AudioEngine::kMaxBlock;
          emit(ok, bus, 0, len, dl, dr, bus == 0 && add);
        }
        done += len;
      }
    } else {
      const int N = fixedSize_;
      for (int i = 0; i < n;) {
        const int take = std::min(n - i, N - fill_);
        float* fin = fifoIn_.data();
        for (int k = 0; k < take; ++k) {
          fin[fill_ + k] = inL ? inL[i + k] : 0.0f;
          fin[N + fill_ + k] = inR ? inR[i + k] : 0.0f;
          fin[2 * N + fill_ + k] = scL ? scL[i + k] : 0.0f;
          fin[3 * N + fill_ + k] = scR ? scR[i + k] : 0.0f;
        }
        for (int bus = 0; bus <= auxCount && bus < fifoBuses_; ++bus) {
          const float* sl = fifoOut_.data() + static_cast<size_t>(bus * 2 * N + fill_);
          const float* sr = sl + N;
          float* dl = bus == 0 ? outL + i : aux + static_cast<size_t>(bus - 1) * 2 * AudioEngine::kMaxBlock + auxPos + i;
          float* dr = bus == 0 ? outR + i : dl + AudioEngine::kMaxBlock;
          for (int k = 0; k < take; ++k) {
            if (bus == 0 && add) {
              dl[k] += sl[k];
              dr[k] += sr[k];
            } else {
              dl[k] = sl[k];
              dr[k] = sr[k];
            }
          }
        }
        fill_ += take;
        i += take;
        if (fill_ == N) {
          fill_ = 0;
          feed(fin, fin + N, fin + 2 * N, fin + 3 * N, N);
          const bool ok = processBlock(inClock_ + i - N, N);
          for (int bus = 0; bus < fifoBuses_; ++bus) {
            float* dl = fifoOut_.data() + static_cast<size_t>(bus * 2 * N);
            emit(ok, bus, 0, N, dl, dl + N, false);
          }
        }
      }
    }
    inClock_ += n;
  }

  // MIDI the plugin sent since the engine last asked (see Generator::takeMidiOut).
  int takeMidiOut(Generator::MidiOutEvent* out, int max) {
    const int n = std::min(max, midiOutCount_);
    for (int i = 0; i < n; ++i) out[i] = midiOut_[i];
    midiOutCount_ = 0;
    return n;
  }

 private:
  struct Pending {
    enum Kind : uint8_t { Event, Param, Mod } kind = Event;
    int64_t at = 0;  // absolute input sample
    NoteEvent ev;
    uint32_t id = 0;
    double value = 0;
  };

  bool queueParam(uint32_t id, double v, int offset, bool mod) {
    if (pendingCount_ >= kMaxPending) return false;
    Pending p;
    p.at = inClock_ + std::max(0, offset);
    p.kind = mod ? Pending::Mod : Pending::Param;
    p.id = id;
    p.value = v;
    insert(p);
    return true;
  }
  void insert(const Pending& p) {  // stable by position
    int i = pendingCount_++;
    while (i > 0 && pending_[i - 1].at > p.at) {
      pending_[i] = pending_[i - 1];
      --i;
    }
    pending_[i] = p;
  }
  void resetSchedule() {
    pendingCount_ = 0;
    fill_ = 0;
    std::fill(fifoIn_.begin(), fifoIn_.end(), 0.0f);
    std::fill(fifoOut_.begin(), fifoOut_.end(), 0.0f);
    midiOutCount_ = 0;
  }

  // Plugin input buffers for the next block.
  void feed(const float* inL, const float* inR, const float* scL, const float* scR, int len) {
    Backend* p = b_.get();
    const int inCh = p->mainInputChannels();
    for (int c = 0; c < inCh; ++c) {
      float* d = p->inputChannel(c);
      if (!d) continue;
      const float* s = c == 0 ? inL : c == 1 ? inR : nullptr;
      if (inCh == 1 && inL && inR)
        for (int i = 0; i < len; ++i) d[i] = 0.5f * (inL[i] + inR[i]);
      else if (s)
        std::memcpy(d, s, sizeof(float) * static_cast<size_t>(len));
      else
        std::memset(d, 0, sizeof(float) * static_cast<size_t>(len));
    }
    const int scCh = p->sidechainChannels();
    for (int c = 0; c < scCh; ++c) {
      float* d = p->sidechainChannel(c);
      if (!d) continue;
      const float* s = c == 0 ? scL : c == 1 ? scR : nullptr;
      if (scCh == 1 && scL && scR)
        for (int i = 0; i < len; ++i) d[i] = 0.5f * (scL[i] + scR[i]);
      else if (s)
        std::memcpy(d, s, sizeof(float) * static_cast<size_t>(len));
      else
        std::memset(d, 0, sizeof(float) * static_cast<size_t>(len));
    }
  }

  // Hands the plugin what is due in [start, start + len) and processes len frames.
  bool processBlock(int64_t start, int len) {
    Backend* p = b_.get();
    int used = 0;
    for (; used < pendingCount_ && pending_[used].at < start + len; ++used) {
      Pending& q = pending_[used];
      const int off = static_cast<int>(std::clamp<int64_t>(q.at - start, 0, len - 1));
      if (q.kind == Pending::Event) {
        NoteEvent e = q.ev;
        e.offset = off;
        p->addEvent(e);
      } else if (q.kind == Pending::Param) {
        p->addParamChange(q.id, q.value, off);
      } else {
        p->addParamMod(q.id, q.value, off);
      }
    }
    if (used > 0) {
      std::memmove(static_cast<void*>(pending_.get()), pending_.get() + used, sizeof(Pending) * static_cast<size_t>(pendingCount_ - used));
      pendingCount_ -= used;
    }
    Transport tr;
    context(start - ctxClock_, tr);
    const int64_t t0 = monoNanos();
    tr.systemNanos = t0;
    const bool ok = p->process(len, tr);
    const int64_t dt = monoNanos() - t0;
    cpuNs_.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, dt)), std::memory_order_relaxed);
    cpuFrames_.fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);
    const double sr = tr.sampleRate > 0 ? tr.sampleRate : 48000.0;
    atomicMax(cpuPeak_, static_cast<float>(static_cast<double>(dt) / (len / sr * 1e9)));
    MidiOut m;
    while (p->popMidiOut(m)) {
      midiOutTotal_.fetch_add(1, std::memory_order_relaxed);
      if (midiOutCount_ >= kMaxMidiOut) continue;
      const int64_t rel = start - ctxClock_ + m.offset;
      midiOut_[midiOutCount_++] = {static_cast<int>(std::clamp<int64_t>(rel, 0, AudioEngine::kMaxBlock - 1)), m.data[0], m.data[1], m.data[2]};
    }
    clock_ += len;
    return ok;
  }

  // Copies (or adds) output bus `bus` of the last block to dl/dr.
  void emit(bool ok, int bus, int from, int len, float* dl, float* dr, bool add) {
    Backend* p = b_.get();
    const int ch = ok ? p->outputChannels(bus) : 0;
    if (ch <= 0) {
      if (!add) {
        std::memset(dl, 0, sizeof(float) * static_cast<size_t>(len));
        std::memset(dr, 0, sizeof(float) * static_cast<size_t>(len));
      }
      return;
    }
    const float* a = p->outputChannel(bus, 0);
    const float* c = ch > 1 ? p->outputChannel(bus, 1) : a;
    if (!a || !c) return;
    a += from;
    c += from;
    if (add) {
      for (int i = 0; i < len; ++i) {
        dl[i] += a[i];
        dr[i] += c[i];
      }
    } else {
      std::memcpy(dl, a, sizeof(float) * static_cast<size_t>(len));
      std::memcpy(dr, c, sizeof(float) * static_cast<size_t>(len));
    }
  }

  // Transport for a plugin block starting `offset` samples after the engine block's start.
  void context(int64_t offset, Transport& out) const {
    const ProcessContext& ctx = ctx_;
    out = Transport{};
    const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    const double tempo = ctx.tempo > 0 ? ctx.tempo : 120.0;
    const double ppq = std::max(1, ctx.ppq);
    const double qnPerSample = tempo / 60.0 / sr;
    double qn = ctx.tick / ppq + (ctx.playing ? static_cast<double>(offset) * qnPerSample : 0.0);
    out.sampleRate = sr;
    out.tempo = tempo;
    out.playing = ctx.playing;
    out.recording = ctx.recording;
    out.tsNum = std::max(1, ctx.tsNum);
    out.tsDen = std::max(1, ctx.tsDen);
    out.looping = ctx.looping && ctx.loopEnd > ctx.loopStart;
    out.loopStartQn = ctx.loopStart / ppq;
    out.loopEndQn = ctx.loopEnd / ppq;
    if (out.looping && qn >= out.loopEndQn) qn = out.loopStartQn + std::fmod(qn - out.loopStartQn, out.loopEndQn - out.loopStartQn);
    const double barQn = 4.0 * out.tsNum / out.tsDen;
    out.projectQn = std::max(0.0, qn);
    out.barNumber = static_cast<int32_t>(std::floor(out.projectQn / barQn + 1e-9));
    out.barQn = out.barNumber * barQn;
    out.projectSamples = static_cast<int64_t>(std::llround(out.projectQn * 60.0 / tempo * sr));
    out.continuousSamples = clock_;
    out.changed = ctx.jumped && offset <= 0;
    out.offline = ctx.offline && opts_.notifyRender;
  }

  int indexOf(uint32_t id) const {
    for (size_t k = 0; k < ids_.size(); ++k)
      if (ids_[k] == id) return static_cast<int>(k);
    return -1;
  }
  void suspend() {
    suspended_.store(true);
    while (busy_.load()) std::this_thread::yield();
  }
  void resume() { suspended_.store(false); }

  std::shared_ptr<const HostedClass> cls_;
  std::unique_ptr<Backend> b_;
  bool sandboxed_ = false;
  HostOptions opts_;
  std::atomic<bool> reload_{false};
  std::vector<uint32_t> ids_;
  std::vector<bool> mapped_, modulated_;
  std::vector<float> sent_;       // audio thread (UI while suspended)
  std::vector<double> sentMod_;   // audio thread: modulation amounts sent
  std::vector<float> ctrlKnown_;  // UI thread
  std::unique_ptr<std::atomic<float>[]> audioVals_;
  std::unique_ptr<std::atomic<float>[]> base_;  // song values of the last program build (modulation)
  std::string lastData_;
  bool loaded_ = false;
  bool readbackPending_ = false;
  bool programCapture_ = false;
  bool crashReported_ = false;
  bool presetsScanned_ = false;
  bool readbackFromSong_ = false;  // the next readback reports songValues_ (state-less load)
  std::vector<float> songValues_;
  std::vector<PresetRef> presets_;
  std::vector<HostEvent> events_;
  std::atomic<bool> suspended_{false}, busy_{false};
  std::thread::id creator_;
  int offlineApplied_ = 0;               // audio thread / UI while suspended
  std::atomic<int> offlineRequest_{-1};  // audio → UI
  // audio-thread scheduling
  ProcessContext ctx_;
  int64_t inClock_ = 0;   // input samples received
  int64_t ctxClock_ = 0;  // inClock_ at the start of the current engine block
  int64_t clock_ = 0;     // samples the plugin processed (continuous time)
  std::unique_ptr<Pending[]> pending_;
  int pendingCount_ = 0;
  int fixedSize_ = 0, fill_ = 0, fifoBuses_ = 0;
  std::vector<float> fifoIn_, fifoOut_;
  std::unique_ptr<Generator::MidiOutEvent[]> midiOut_;
  int midiOutCount_ = 0;
  std::atomic<uint64_t> midiOutTotal_{0};
  std::atomic<uint64_t> cpuNs_{0}, cpuFrames_{0};
  std::atomic<float> cpuPeak_{0};
};

// ---------------------------------------------------------------- instrument

class HostedGenerator final : public Generator {
 public:
  explicit HostedGenerator(std::unique_ptr<Core> core) : core_(std::move(core)) {
    // Extra outputs as scanned (some formats only know them once prepared).
    const int scanned = static_cast<int>(core_->hostedClass().auxOutputs.size());
    auxCount_ = std::min(std::max(core_->backend()->auxOutputCount(), scanned), kMaxAuxOutputs);
    aux_.assign(static_cast<size_t>(auxCount_) * 2 * AudioEngine::kMaxBlock, 0.0f);
  }
  bool active() const override { return true; }  // tails are the plugin's business
  bool supportsGlide() const override { return core_->backend()->supportsTuning(); }
  void prepareHost(const PluginState& state, double sampleRate) override { core_->prepareHost(state, sampleRate); }
  int latencySamples() const override { return core_->totalLatency(); }
  bool stale() const override { return core_->stale(); }
  int auxOutputCount() const override { return auxCount_; }
  bool auxOutput(int k, const float*& L, const float*& R) const override {
    if (k < 0 || k >= auxCount_) return false;
    L = aux_.data() + static_cast<size_t>(k) * 2 * AudioEngine::kMaxBlock;
    R = L + AudioEngine::kMaxBlock;
    return true;
  }
  int takeMidiOut(MidiOutEvent* out, int max) override { return core_->takeMidiOut(out, max); }
  Core& core() { return *core_; }

 protected:
  bool wholeBlock() const override { return true; }
  bool handlesNotePan() const override { return core_->backend()->handlesNotePan(); }
  void onNoteOn(uint32_t id, double pitch, float velocity) override { startNote(id, pitch, velocity, 0, nullptr); }
  void onNoteOff(uint32_t id) override { stopNote(id, 0.5f); }
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override {
    startNote(id, pitch, velocity, std::clamp(props.color, 0, 15), &props);
  }
  void onCut(uint32_t id) override { stopNote(id, 0.0f); }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    Note* n = find(id);
    if (!n || !core_->backend()->supportsTuning()) return;
    n->glideTo = pitch;
    n->glideLeft = std::max<int64_t>(1, static_cast<int64_t>(seconds * sampleRate_));
    n->glideStep = (pitch - n->cur) / static_cast<double>(n->glideLeft);
    n->glideFrom = eventOffset();
  }
  void onMidi(uint8_t s, uint8_t a, uint8_t b) override {
    NoteEvent e;
    e.type = NoteEvent::Midi;
    e.midi[0] = s;
    e.midi[1] = a;
    e.midi[2] = b;
    e.channel = static_cast<int8_t>(s & 0x0F);
    queue(e, eventOffset());
  }
  void onAllOff() override {
    for (auto& n : notes_)
      if (n.used) stopNote(n.id, 0.5f);
  }
  void onKill() override { onAllOff(); }
  void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) override {
    (void)aux;
    sampleRate_ = ctx.sampleRate;
    if (entered_) core_->leave();  // (a block that never rendered)
    entered_ = core_->enter();
    if (entered_) {
      core_->beginBlock(ctx);
      core_->sendParams(state);
      for (int i = 0; i < deferredCount_; ++i) core_->queueEvent(deferred_[i], 0);
      deferredCount_ = 0;
    }
  }
  // One call per block (wholeBlock): the events of the block are already
  // queued with their offsets; Core splits the block at them when the
  // "process maximum size buffers" option is off.
  void render(float* L, float* R, int n) override {
    if (!entered_) {
      clearAux(0, n);
      return;
    }
    emitGlides(n);
    core_->run(n, nullptr, nullptr, nullptr, nullptr, L, R, true, aux_.data(), auxCount_, 0);
    entered_ = false;
    core_->leave();
  }

 private:
  struct Note {
    bool used = false;
    uint32_t id = 0;
    int32_t noteId = 0;
    int16_t key = 60;
    int8_t channel = 0;
    double base = 60, cur = 60;  // started pitch, current pitch
    double glideTo = 60, glideStep = 0;
    int64_t glideLeft = 0;
    int glideFrom = 0;  // offset in the block where the glide starts
  };
  Note* find(uint32_t id) {
    for (auto& n : notes_)
      if (n.used && n.id == id) return &n;
    return nullptr;
  }
  void queue(const NoteEvent& e, int offset) {
    if (entered_)
      core_->queueEvent(e, offset);
    else if (deferredCount_ < 256)
      deferred_[deferredCount_++] = e;
  }
  void clearAux(int from, int n) {
    for (int k = 0; k < auxCount_; ++k) {
      float* dl = aux_.data() + static_cast<size_t>(k) * 2 * AudioEngine::kMaxBlock + from;
      std::memset(dl, 0, sizeof(float) * static_cast<size_t>(n));
      std::memset(dl + AudioEngine::kMaxBlock, 0, sizeof(float) * static_cast<size_t>(n));
    }
  }
  void startNote(uint32_t id, double pitch, float velocity, int channel, const NoteProps* props) {
    Note* slot = nullptr;
    for (auto& n : notes_)
      if (!n.used) {
        slot = &n;
        break;
      }
    if (!slot) {  // table full: release the oldest
      stopNote(notes_[0].id, 0.5f);
      slot = &notes_[0];
    }
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    *slot = Note{};
    slot->used = true;
    slot->id = id;
    slot->noteId = nextNoteId_;
    nextNoteId_ = (nextNoteId_ + 1) & 0x7fffffff;
    slot->key = static_cast<int16_t>(key);
    slot->channel = static_cast<int8_t>(channel);
    slot->base = slot->cur = slot->glideTo = pitch;
    NoteEvent e;
    e.type = NoteEvent::NoteOn;
    e.noteId = slot->noteId;
    e.key = slot->key;
    e.channel = slot->channel;
    e.velocity = std::clamp(velocity, 0.0f, 1.0f);
    e.tuning = pitch - key;
    if (props) {
      e.hasProps = true;
      e.pan = props->pan;
      e.modX = props->modX;
      e.modY = props->modY;
    }
    queue(e, eventOffset());
  }
  void stopNote(uint32_t id, float velocity) {
    Note* n = find(id);
    if (!n) return;
    NoteEvent e;
    e.type = NoteEvent::NoteOff;
    e.noteId = n->noteId;
    e.key = n->key;
    e.channel = n->channel;
    e.velocity = velocity;
    e.tuning = n->base - n->key;
    queue(e, eventOffset());
    n->used = false;
  }
  // Glides: one tuning value every 32 samples of the block (render span n).
  void emitGlides(int n) {
    for (auto& nt : notes_) {
      if (!nt.used || nt.glideLeft <= 0) continue;
      for (int pos = std::min(nt.glideFrom, n - 1); pos < n && nt.glideLeft > 0; pos += 32) {
        const int64_t step = std::min<int64_t>(nt.glideLeft, std::min(32, n - pos));
        nt.glideLeft -= step;
        nt.cur = nt.glideLeft == 0 ? nt.glideTo : nt.cur + nt.glideStep * static_cast<double>(step);
        NoteEvent e;
        e.type = NoteEvent::Tuning;
        e.noteId = nt.noteId;
        e.key = nt.key;
        e.channel = nt.channel;
        e.tuning = nt.cur - nt.base;
        queue(e, pos);
      }
      nt.glideFrom = 0;
    }
  }

  std::unique_ptr<Core> core_;
  double sampleRate_ = 44100;
  NoteEvent deferred_[256];  // events from outside a block (kill, panic): sent with the next one
  int deferredCount_ = 0;
  bool entered_ = false;
  Note notes_[256];
  int32_t nextNoteId_ = 1;
  int auxCount_ = 0;
  std::vector<float> aux_;  // [output][L, R][kMaxBlock]
};

// ---------------------------------------------------------------- effect

class HostedEffect final : public Effect {
 public:
  explicit HostedEffect(std::unique_ptr<Core> core) : core_(std::move(core)) {}
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    run(state, ctx, L, R, n);
  }
  void processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    (void)aux;
    run(state, ctx, L, R, n);
  }
  void reset() override {}
  void setSideIo(const SideIo& io) override { io_ = io; }
  void prepareHost(const PluginState& state, double sampleRate) override { core_->prepareHost(state, sampleRate); }
  int latencySamples() const override { return core_->totalLatency(); }
  bool stale() const override { return core_->stale(); }
  Core& core() { return *core_; }

 private:
  void run(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) {
    Backend* p = core_->backend();
    if (p->crashed()) {  // a crashed sandboxed plugin is silent
      std::memset(L, 0, sizeof(float) * static_cast<size_t>(n));
      std::memset(R, 0, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    if (!core_->enter()) return;  // reconfiguring: pass through
    core_->beginBlock(ctx);
    core_->sendParams(state);
    core_->run(n, L, R, io_.scL, io_.scR, L, R, false, nullptr, 0, 0);
    core_->leave();
  }

  std::unique_ptr<Core> core_;
  SideIo io_;
};

std::unique_ptr<Core> makeCore(const std::string& type) {
  auto cls = hostedClass(type);
  if (!cls) {
    setError("plugin not installed or not scanned: " + type);
    return nullptr;
  }
  std::string err;
  const bool sandbox = runsInSandbox(type);
  std::unique_ptr<Backend> b = sandbox ? createSandboxBackend(childExecutable(), cls->format, cls->bundle, cls->uid, &err, cls->name)
                                       : createBackend(cls->format, cls->bundle, cls->uid, &err);
  if (!b) {
    setError(cls->name + ": " + err);
    return nullptr;
  }
  return std::make_unique<Core>(cls, std::move(b), sandbox);
}

}  // namespace

std::unique_ptr<Generator> createHostedGenerator(const std::string& type) {
  auto core = makeCore(type);
  if (!core) return nullptr;
  return std::make_unique<HostedGenerator>(std::move(core));
}

std::unique_ptr<Effect> createHostedEffect(const std::string& type) {
  auto core = makeCore(type);
  if (!core) return nullptr;
  return std::make_unique<HostedEffect>(std::move(core));
}

HostedInstance* hostedInstance(Generator* g) {
  auto* h = dynamic_cast<HostedGenerator*>(g);
  return h ? &h->core() : nullptr;
}

HostedInstance* hostedInstance(Effect* fx) {
  auto* h = dynamic_cast<HostedEffect*>(fx);
  return h ? &h->core() : nullptr;
}

// ---------------------------------------------------------------- scanning service

namespace {
struct Service {
  std::mutex lock;
  std::string cacheFile;
  ScanCache cache;
  ScanStatus status;
  std::thread worker;
};
Service& service() {
  static auto* s = new Service;  // never destroyed (the worker may outlive main)
  return *s;
}
int countFailed(const ScanCache& c) {
  int n = 0;
  for (const auto& e : c.entries()) n += e.ok ? 0 : 1;
  return n;
}
int countClasses() {
  std::lock_guard<std::mutex> g(gClassLock);
  return static_cast<int>(classMap().size());
}

// Entries of every enabled format: default folders + user folders.
std::vector<std::pair<std::string, std::string>> discoverAll() {
  PluginPrefs prefs = pluginPrefs();
  std::vector<std::pair<std::string, std::string>> out;
  for (const Format& f : formats()) {
    if (prefs.disabledFormats.count(f.id)) continue;
    std::vector<std::string> folders = f.defaultPaths();
    if (std::strcmp(f.id, "au") != 0)
      for (const auto& x : prefs.extraFolders)
        if (std::find(folders.begin(), folders.end(), x) == folders.end()) folders.push_back(x);
    for (auto& e : f.discover(folders)) out.push_back({f.id, e});
  }
  return out;
}

void finishEntry(Service& sv, ScanResult r) {
  registerScanResult(r);
  std::lock_guard<std::mutex> g2(sv.lock);
  sv.cache.put(std::move(r));
  sv.cache.save();
  sv.status.classes = countClasses();
  sv.status.failed = countFailed(sv.cache);
  sv.status.generation = gGeneration.load();
}
}  // namespace

void initHosting(const std::string& configDir) {
  loadPluginPrefs(configDir);
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  s.cacheFile = configDir + "/plugin-cache.json";
  s.cache = ScanCache(s.cacheFile);
  s.cache.load();
  for (const auto& e : s.cache.entries())
    if (findFormat(e.format)) registerScanResult(e);
  s.status.classes = countClasses();
  s.status.failed = countFailed(s.cache);
  s.status.generation = gGeneration.load();
}

void startScan(const std::string& exe, bool force) {
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  if (s.status.running || exe.empty()) return;
  if (s.worker.joinable()) s.worker.join();
  s.status.running = true;
  s.status.done = s.status.total = 0;
  s.worker = std::thread([exe, force]() {
    Service& sv = service();
    std::vector<std::pair<std::string, std::string>> found = discoverAll();
    std::vector<std::pair<std::string, std::string>> todo;
    {
      std::lock_guard<std::mutex> g2(sv.lock);
      for (const Format& f : formats()) {
        std::vector<std::string> mine;
        for (const auto& [fid, e] : found)
          if (fid == f.id) mine.push_back(e);
        for (auto& e : force ? mine : sv.cache.stale(mine, f.id)) todo.push_back({f.id, e});
      }
      // Forget entries that were removed (or whose format was switched off).
      std::vector<std::pair<std::string, std::string>> gone;
      for (const auto& e : sv.cache.entries())
        if (std::find(found.begin(), found.end(), std::make_pair(e.format, e.bundle)) == found.end()) gone.push_back({e.format, e.bundle});
      for (const auto& [fid, b] : gone) sv.cache.remove(b, fid);
      sv.status.total = static_cast<int>(todo.size());
    }
    for (const auto& [fid, e] : todo) {
      {
        std::lock_guard<std::mutex> g2(sv.lock);
        sv.status.current = e;
      }
      finishEntry(sv, scanIsolated(exe, fid, e));
      std::lock_guard<std::mutex> g2(sv.lock);
      ++sv.status.done;
    }
    std::lock_guard<std::mutex> g2(sv.lock);
    sv.cache.save();
    sv.status.current.clear();
    sv.status.running = false;
    sv.status.generation = gGeneration.load();
  });
}

void rescanEntry(const std::string& exe, const std::string& format, const std::string& entry) {
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  if (s.status.running || exe.empty()) return;
  if (s.worker.joinable()) s.worker.join();
  s.status.running = true;
  s.status.done = 0;
  s.status.total = 1;
  s.worker = std::thread([exe, format, entry]() {
    Service& sv = service();
    {
      std::lock_guard<std::mutex> g2(sv.lock);
      sv.status.current = entry;
    }
    finishEntry(sv, scanIsolated(exe, format, entry));
    std::lock_guard<std::mutex> g2(sv.lock);
    sv.status.done = 1;
    sv.status.current.clear();
    sv.status.running = false;
  });
}

ScanStatus scanStatus() {
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  return s.status;
}

std::vector<std::pair<std::string, std::string>> blacklist() {
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& e : s.cache.entries())
    if (!e.ok) out.push_back({e.bundle, e.error});
  return out;
}

std::vector<CacheEntry> cacheEntries() {
  Service& s = service();
  std::lock_guard<std::mutex> g(s.lock);
  std::vector<CacheEntry> out;
  for (const auto& e : s.cache.entries()) out.push_back({e.format, e.bundle, e.error, e.ok, static_cast<int>(e.classes.size())});
  return out;
}

void stopScan() {
  Service& s = service();
  std::thread t;
  {
    std::lock_guard<std::mutex> g(s.lock);
    t = std::move(s.worker);
  }
  if (t.joinable()) t.join();
}

}  // namespace gl::host

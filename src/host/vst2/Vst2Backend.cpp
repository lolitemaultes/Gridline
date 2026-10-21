// Gridline — VST2-style plugin hosting (format "vst2") over the clean-room ABI
// description in Vst2Abi.h. Types: "vst2:<8 hex digits unique id>".
//
// State: plugins that keep their state as a chunk → "vst2c1:<base64 bank
// chunk>"; others → "vst2p1:<base64: u32 program, f32 parameter values>".
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include "../Dylib.h"
#include "../PluginScan.h"
#include "Vst2Abi.h"
#include "../../core/Platform.h"

namespace fs = std::filesystem;

namespace gl::host {

namespace {
namespace abi = vst2abi;

thread_local int32_t tlsShellId = 0;  // sub-plugin requested from a shell (hostCurrentId)
thread_local bool tlsAudio = false;

uint32_t fromHex8(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16)); }
std::string text(const char* buf, size_t cap) { return std::string(buf, strnlen(buf, cap)); }

abi::EntryFn entryOf(const std::string& path, std::string* err) {
  auto lib = Dylib::open(path, err);
  if (!lib) return nullptr;
  void* sym = lib->function("VSTPluginMain");
  if (!sym) sym = lib->function("main_macho");
  if (!sym) sym = lib->function("main");
  if (!sym) {
    if (err) *err = "no plugin entry point";
    return nullptr;
  }
  return toFunction<abi::EntryFn>(sym);
}

class Vst2Backend;

class Vst2Editor final : public Editor {
 public:
  explicit Vst2Editor(Vst2Backend* b) : b_(b) {}
  ~Vst2Editor() override;
  bool size(int& w, int& h) override;
  bool attach(void* parent) override;
  void idle() override;
  bool hostKey(bool down, int usage, uint32_t ch, uint32_t mods) override;
  Vst2Backend* b_;
  bool open = false;
};

class Vst2Backend final : public Backend {
 public:
  static std::unique_ptr<Vst2Backend> create(const std::string& path, uint32_t id, std::string* err) {
    abi::EntryFn entry = entryOf(path, err);
    if (!entry) return nullptr;
    std::unique_ptr<Vst2Backend> b(new Vst2Backend);
    tlsShellId = static_cast<int32_t>(id);
    abi::Effect* fx = entry(&Vst2Backend::hostCallback);
    tlsShellId = 0;
    if (!fx || fx->magic != abi::kMagic) {
      if (err) *err = "the entry point returned no plugin";
      return nullptr;
    }
    b->fx_ = fx;
    fx->hostData = b.get();
    b->dispatch(abi::opOpen);
    b->readInfo();
    return b;
  }
  ~Vst2Backend() override {
    if (!fx_) return;
    if (editor_) editor_->b_ = nullptr;
    closeEditor();
    deactivate();
    dispatch(abi::opClose);
  }

  abi::Effect* effect() const { return fx_; }
  intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0) const {
    return fx_->dispatch(fx_, op, index, value, ptr, opt);
  }

  // ---- description ----
  const char* format() const override { return "VST2"; }
  const std::vector<ParamInfo>& params() const override { return params_; }
  bool hasEventInput() const override { return eventInput_; }
  int mainInputChannels() const override { return std::min(2, std::max(0, fx_->inputCount)); }
  int mainOutputChannels() const override { return std::min(2, std::max(0, fx_->outputCount)); }
  int sidechainChannels() const override { return fx_->inputCount > 2 ? std::min(2, fx_->inputCount - 2) : 0; }
  bool hasEventOutput() const override { return eventOutput_; }
  bool hasBypass() const override { return softBypass_; }
  int auxOutputCount() const override { return instrument_ ? std::max(0, (fx_->outputCount - 1) / 2) : 0; }
  std::string auxOutputName(int k) const override {
    abi::PinInfo pin{};
    if (dispatch(abi::opGetOutputInfo, 2 + 2 * k, 0, &pin) == 1 && pin.label[0]) return text(pin.label, sizeof pin.label);
    return Backend::auxOutputName(k);
  }

  // ---- UI thread ----
  bool prepare(double sr, int maxBlock) override {
    if (active_ && sr == sampleRate_ && maxBlock == maxBlock_) return true;
    deactivate();
    sampleRate_ = sr;
    maxBlock_ = std::max(1, maxBlock);
    // Every pointer the plugin may read is valid: instruments that look at
    // inputs[0..1] get silence, plugins writing two outputs when they
    // declare one write into scratch.
    const int nIn = std::max(0, fx_->inputCount), nOut = std::max(0, fx_->outputCount);
    const int inSlots = std::max(2, nIn), outSlots = std::max(2, nOut);
    storage_.assign(static_cast<size_t>(inSlots + outSlots) * static_cast<size_t>(maxBlock_), 0.0f);
    in_.assign(static_cast<size_t>(inSlots), nullptr);
    out_.assign(static_cast<size_t>(outSlots), nullptr);
    for (int i = 0; i < inSlots; ++i) in_[static_cast<size_t>(i)] = storage_.data() + static_cast<size_t>(i) * static_cast<size_t>(maxBlock_);
    for (int i = 0; i < outSlots; ++i)
      out_[static_cast<size_t>(i)] = storage_.data() + static_cast<size_t>(inSlots + i) * static_cast<size_t>(maxBlock_);
    dispatch(abi::opSetSampleRate, 0, 0, nullptr, static_cast<float>(sr));
    dispatch(abi::opSetBlockSize, 0, maxBlock_);
    dispatch(abi::opSetPrecision, 0, 0);
    dispatch(abi::opPowerChanged, 0, 1);
    dispatch(abi::opStartProcess);
    active_ = true;
    latency_.store(std::max(0, fx_->latency));
    return true;
  }
  double sampleRate() const override { return sampleRate_; }
  int maxBlock() const override { return maxBlock_; }
  bool active() const override { return active_; }
  void restart(int32_t flags) override {
    if (flags & (kRestartReload | kRestartLatency)) {
      double sr = sampleRate_;
      int mb = maxBlock_;
      deactivate();
      if (sr > 0) prepare(sr, mb);
    }
    if (flags & kRestartParamInfo) readParams();
  }
  int latency() const override { return latency_.load(); }
  int tailSamples() const override { return static_cast<int>(std::clamp<intptr_t>(dispatch(abi::opGetTailSize), 0, 1 << 30)); }
  bool getState(std::string& data) override {
    if (fx_->flags & abi::kFlagChunks) {
      void* chunk = nullptr;
      intptr_t n = dispatch(abi::opGetChunk, 0, 0, &chunk);
      if (n > 0 && chunk && n < (intptr_t(1) << 30)) {
        data = "vst2c1:" + base64Encode(std::string(static_cast<const char*>(chunk), static_cast<size_t>(n)));
        return true;
      }
      // An empty chunk: fall back to the parameter list.
    }
    std::string bytes;
    auto put = [&](const void* p, size_t n) { bytes.append(static_cast<const char*>(p), n); };
    uint32_t prog = static_cast<uint32_t>(std::max<intptr_t>(0, dispatch(abi::opGetProgram)));
    put(&prog, 4);
    for (int i = 0; i < fx_->paramCount; ++i) {
      float v = fx_->getParam(fx_, i);
      put(&v, 4);
    }
    data = "vst2p1:" + base64Encode(bytes);
    return true;
  }
  bool setState(const std::string& data) override {
    std::string bytes;
    if (data.compare(0, 7, "vst2c1:") == 0) {
      if (!base64Decode(data.substr(7), bytes) || bytes.empty()) return false;
      dispatch(abi::opBeginSetProgram);
      dispatch(abi::opSetChunk, 0, static_cast<intptr_t>(bytes.size()), bytes.data());
      dispatch(abi::opEndSetProgram);
      return true;
    }
    if (data.compare(0, 7, "vst2p1:") == 0) {
      if (!base64Decode(data.substr(7), bytes) || bytes.size() < 4) return false;
      uint32_t prog = 0;
      std::memcpy(&prog, bytes.data(), 4);
      if (fx_->programCount > 0 && prog < static_cast<uint32_t>(fx_->programCount)) dispatch(abi::opSetProgram, 0, prog);
      const size_t n = std::min<size_t>((bytes.size() - 4) / 4, static_cast<size_t>(std::max(0, fx_->paramCount)));
      for (size_t i = 0; i < n; ++i) {
        float v = 0;
        std::memcpy(&v, bytes.data() + 4 + 4 * i, 4);
        fx_->setParam(fx_, static_cast<int32_t>(i), std::clamp(v, 0.0f, 1.0f));
      }
      return true;
    }
    return false;
  }
  double paramNormalized(uint32_t id) const override {
    return id < static_cast<uint32_t>(std::max(0, fx_->paramCount)) ? std::clamp<double>(fx_->getParam(fx_, static_cast<int32_t>(id)), 0.0, 1.0) : 0.0;
  }
  std::string paramText(uint32_t id, double n) const override {
    if (id >= static_cast<uint32_t>(std::max(0, fx_->paramCount))) return {};
    // The format only describes the current value.
    if (std::fabs(fx_->getParam(fx_, static_cast<int32_t>(id)) - n) > 1e-4) return {};
    char buf[256] = {}, unit[256] = {};
    dispatch(abi::opGetParamText, static_cast<int32_t>(id), 0, buf);
    dispatch(abi::opGetParamUnit, static_cast<int32_t>(id), 0, unit);
    std::string t = text(buf, sizeof buf), u = text(unit, sizeof unit);
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    return u.empty() || t.empty() ? t : t + " " + u;
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
  }
  int programCount() const override { return programs_.size() > 1 ? static_cast<int>(programs_.size()) : 0; }
  std::string programName(int i) const override {
    return i >= 0 && static_cast<size_t>(i) < programs_.size() ? programs_[static_cast<size_t>(i)] : std::string();
  }
  bool selectProgram(int i) override {
    if (i < 0 || i >= fx_->programCount) return false;
    dispatch(abi::opBeginSetProgram);
    dispatch(abi::opSetProgram, 0, i);
    dispatch(abi::opEndSetProgram);
    return true;
  }
  bool hasEditor() const override { return (fx_->flags & abi::kFlagHasEditor) != 0; }
  std::unique_ptr<Editor> createEditor() override {
    if (!hasEditor() || editor_) return nullptr;
    auto e = std::make_unique<Vst2Editor>(this);
    editor_ = e.get();
    return e;
  }
  void closeEditor() {
    if (editor_ && editor_->open) dispatch(abi::opEditorClose);
    if (editor_) editor_->open = false;
    editor_ = nullptr;
  }

  // ---- audio thread ----
  float* inputChannel(int ch) override { return ch >= 0 && ch < mainInputChannels() ? in_[static_cast<size_t>(ch)] : nullptr; }
  bool popMidiOut(MidiOut& m) override { return midiOut_.pop(m); }
  float* sidechainChannel(int ch) override { return ch >= 0 && ch < sidechainChannels() ? in_[static_cast<size_t>(2 + ch)] : nullptr; }
  int outputChannels(int bus) const override {
    if (bus == 0) return mainOutputChannels();
    const int first = 2 * bus;
    return bus > 0 && bus <= auxOutputCount() ? std::min(2, fx_->outputCount - first) : 0;
  }
  const float* outputChannel(int bus, int ch) override {
    if (ch < 0 || ch >= outputChannels(bus)) return nullptr;
    return out_[static_cast<size_t>(2 * bus + ch)];
  }
  bool addParamChange(uint32_t id, double v, int offset) override {
    (void)offset;  // applied at the block start
    if (pcount_ >= kMaxChanges) return false;
    changes_[pcount_++] = {id, static_cast<float>(std::clamp(v, 0.0, 1.0))};
    return true;
  }
  void addEvent(const NoteEvent& n) override {
    if (ecount_ >= kMaxEvents) return;
    abi::MidiEvent& e = midi_[ecount_];
    e = abi::MidiEvent{};
    e.type = 1;
    e.byteSize = sizeof(abi::MidiEvent);
    e.frameOffset = n.offset;
    const uint8_t ch = static_cast<uint8_t>(n.channel & 0x0F);
    switch (n.type) {
      case NoteEvent::NoteOn:
        e.data[0] = static_cast<uint8_t>(0x90 | ch);
        e.data[1] = static_cast<uint8_t>(n.key & 0x7F);
        e.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), 1, 127));
        e.detune = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(n.tuning * 100)), -64, 63));
        break;
      case NoteEvent::NoteOff:
        e.data[0] = static_cast<uint8_t>(0x80 | ch);
        e.data[1] = static_cast<uint8_t>(n.key & 0x7F);
        e.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), 0, 127));
        break;
      case NoteEvent::Tuning:
        return;  // no per-note tuning in this format
      case NoteEvent::Midi:
        std::memcpy(e.data, n.midi, 3);
        break;
    }
    ++ecount_;
  }
  void clearInput() override {
    pcount_ = 0;
    ecount_ = 0;
  }
  bool process(int n, const Transport& t) override {
    if (!active_ || n <= 0 || n > maxBlock_) {
      for (float* o : out_)
        if (o) std::memset(o, 0, sizeof(float) * static_cast<size_t>(std::max(0, std::min(n, maxBlock_))));
      clearInput();
      return false;
    }
    tlsAudio = true;
    offlineNow_ = t.offline || offline_;
    midiOut_.clear();
    inProcess_ = true;
    if (const int bp = pendingBypass_.exchange(-1); bp >= 0) dispatch(abi::opSetBypass, 0, bp);
    for (int i = 0; i < pcount_; ++i) fx_->setParam(fx_, static_cast<int32_t>(changes_[i].id), changes_[i].value);
    pcount_ = 0;
    time_ = abi::TimeInfo{};
    time_.samplePos = static_cast<double>(t.projectSamples);
    time_.sampleRate = t.sampleRate;
    time_.systemNanos = static_cast<double>(t.systemNanos);
    time_.ppqPos = t.projectQn;
    time_.tempo = t.tempo;
    time_.barStartPpq = t.barQn;
    time_.cycleStartPpq = t.loopStartQn;
    time_.cycleEndPpq = t.loopEndQn;
    time_.sigNumerator = t.tsNum;
    time_.sigDenominator = t.tsDen;
    {  // samples to the next MIDI clock (24 per quarter note)
      const double clockQn = 1.0 / 24.0;
      const double next = std::ceil(t.projectQn / clockQn - 1e-9) * clockQn;
      time_.samplesToClock = static_cast<int32_t>(std::lround((next - t.projectQn) * 60.0 / std::max(1.0, t.tempo) * t.sampleRate));
    }
    time_.flags = abi::timePpqValid | abi::timeTempoValid | abi::timeBarsValid | abi::timeSigValid | abi::timeNanosValid | abi::timeClockValid |
                  abi::timeCyclePosValid | (t.playing ? abi::timePlaying : 0) | (t.recording ? abi::timeRecording : 0) |
                  (t.looping ? abi::timeCycleActive : 0) | (t.changed ? abi::timeChanged : 0);
    if (ecount_ > 0) {
      for (int i = 1; i < ecount_; ++i) {  // stable insertion sort by offset (no allocation)
        abi::MidiEvent e = midi_[i];
        int j = i;
        while (j > 0 && midi_[j - 1].frameOffset > e.frameOffset) {
          midi_[j] = midi_[j - 1];
          --j;
        }
        midi_[j] = e;
      }
      list_.count = ecount_;
      for (int i = 0; i < ecount_; ++i) list_.events[i] = &midi_[i];
      dispatch(abi::opProcessEvents, 0, 0, &list_);
    }
    for (float* o : out_)
      if (o) std::memset(o, 0, sizeof(float) * static_cast<size_t>(n));
    for (size_t i = static_cast<size_t>(std::max(0, fx_->inputCount)); i < in_.size(); ++i)  // scratch inputs stay silent
      std::memset(in_[i], 0, sizeof(float) * static_cast<size_t>(n));
    if (fx_->processReplacing && (fx_->flags & abi::kFlagCanReplace)) fx_->processReplacing(fx_, in_.data(), out_.data(), n);
    else if (fx_->processAccumulating) fx_->processAccumulating(fx_, in_.data(), out_.data(), n);
    ecount_ = 0;
    inProcess_ = false;
    tlsAudio = false;
    return true;
  }
  bool setOffline(bool offline) override {
    offline_ = offline;
    return true;
  }
  bool setBypass(bool on) override {
    if (!softBypass_) return false;
    if (active_)
      pendingBypass_.store(on ? 1 : 0);
    else
      dispatch(abi::opSetBypass, 0, on ? 1 : 0);
    return true;
  }

  static intptr_t hostCallback(abi::Effect* fx, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
    Vst2Backend* b = fx ? static_cast<Vst2Backend*>(fx->hostData) : nullptr;
    switch (op) {
      case abi::hostVersion: return 2400;
      case abi::hostCurrentId: return tlsShellId;
      case abi::hostAutomate:
        if (b) b->events_.push({HostEvent::PerformEdit, static_cast<uint32_t>(index), std::clamp<double>(opt, 0.0, 1.0), 0});
        return 0;
      case abi::hostBeginEdit:
        if (b) b->events_.push({HostEvent::BeginEdit, static_cast<uint32_t>(index), 0, 0});
        return 0;
      case abi::hostEndEdit:
        if (b) b->events_.push({HostEvent::EndEdit, static_cast<uint32_t>(index), 0, 0});
        return 0;
      case abi::hostGetTime: return b ? reinterpret_cast<intptr_t>(&b->time_) : 0;
      case abi::hostIoChanged:
        if (b) b->restartFlags_ |= kRestartReload;
        return 1;
      case abi::hostUpdateDisplay:
        if (b) b->restartFlags_ |= kRestartParamValues;
        return 1;
      case abi::hostResizeWindow:
        return b && b->editor_ && b->editor_->requestResize && b->editor_->requestResize(index, static_cast<int>(value)) ? 1 : 0;
      case abi::hostGetSampleRate: return b ? static_cast<intptr_t>(b->sampleRate_) : 44100;
      case abi::hostGetBlockSize: return b ? b->maxBlock_ : 512;
      case abi::hostGetProcessLevel:
        if (b && b->offlineNow_) return abi::levelOffline;
        return tlsAudio ? abi::levelRealtime : abi::levelUser;
      case abi::hostProcessEvents:  // the plugin's MIDI output
        if (b && ptr && b->inProcess_) {
          const auto* list = static_cast<const abi::EventList<1>*>(ptr);
          const int n = std::clamp(list->count, 0, 4096);
          for (int i = 0; i < n; ++i) {
            const auto* ev = static_cast<const abi::MidiEvent*>(list->events[i]);
            if (ev && ev->type == 1) b->midiOut_.push(ev->frameOffset, ev->data[0], ev->data[1], ev->data[2]);
          }
        }
        return 1;
      case abi::hostGetAutomationState: return 1;
      case abi::hostGetLanguage: return 1;
      case abi::hostWantsMidi: return 1;
      case abi::hostIdle: return 0;
      case abi::hostGetVendor:
        if (ptr) std::strcpy(static_cast<char*>(ptr), "Gridline");
        return 1;
      case abi::hostGetProduct:
        if (ptr) std::strcpy(static_cast<char*>(ptr), "Gridline");
        return 1;
      case abi::hostGetVendorVersion: return 500;
      case abi::hostCanDo: {
        const char* s = static_cast<const char*>(ptr);
        if (!s) return 0;
        for (const char* yes : {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "sizeWindow", "supplyIdle", "receiveVstEvents",
                                "receiveVstMidiEvent", "sendVstMidiEventFlagIsRealtime", "startStopProcess", "shellCategory", "acceptIOChanges",
                                "supportShell"})
          if (!std::strcmp(s, yes)) return 1;
        return 0;
      }
      default: return 0;
    }
  }

  Vst2Editor* editor_ = nullptr;

 private:
  struct Change {
    uint32_t id;
    float value;
  };
  static constexpr int kMaxChanges = 2048, kMaxEvents = 1024;
  Vst2Backend() : changes_(new Change[kMaxChanges]), midi_(new abi::MidiEvent[kMaxEvents]) {}

  void readInfo() {
    const intptr_t cat = dispatch(abi::opGetCategory);
    instrument_ = (fx_->flags & abi::kFlagInstrument) != 0 || cat == abi::catInstrument;
    eventInput_ = instrument_ || dispatch(abi::opCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent")) == 1 ||
                  dispatch(abi::opCanDo, 0, 0, const_cast<char*>("receiveVstEvents")) == 1;
    eventOutput_ = dispatch(abi::opCanDo, 0, 0, const_cast<char*>("sendVstMidiEvent")) == 1 ||
                   dispatch(abi::opCanDo, 0, 0, const_cast<char*>("sendVstEvents")) == 1;
    softBypass_ = dispatch(abi::opCanDo, 0, 0, const_cast<char*>("bypass")) == 1;
    readParams();
    programs_.clear();
    for (int i = 0; i < std::min(fx_->programCount, 1024); ++i) {
      char buf[256] = {};
      if (dispatch(abi::opGetProgramNameIndexed, i, -1, buf) != 1 || !buf[0]) std::snprintf(buf, sizeof buf, "Program %d", i + 1);
      programs_.push_back(text(buf, sizeof buf));
    }
  }
  void readParams() {
    params_.clear();
    for (int i = 0; i < fx_->paramCount; ++i) {
      ParamInfo p;
      p.id = static_cast<uint32_t>(i);
      char buf[256] = {}, unit[256] = {};
      dispatch(abi::opGetParamName, i, 0, buf);
      dispatch(abi::opGetParamUnit, i, 0, unit);
      p.title = text(buf, sizeof buf);
      if (p.title.empty()) p.title = "Param " + std::to_string(i + 1);
      p.units = text(unit, sizeof unit);
      p.def = std::clamp<double>(fx_->getParam(fx_, i), 0.0, 1.0);
      p.flags = kParamAutomatable;
      params_.push_back(std::move(p));
    }
    indexParams();
  }
  void deactivate() {
    if (!active_) return;
    dispatch(abi::opStopProcess);
    dispatch(abi::opPowerChanged, 0, 0);
    active_ = false;
  }

  abi::Effect* fx_ = nullptr;
  std::vector<ParamInfo> params_;
  std::vector<std::string> programs_;
  bool instrument_ = false, eventInput_ = false, eventOutput_ = false, softBypass_ = false;
  bool offline_ = false;     // UI: rendering offline
  bool offlineNow_ = false;  // audio: this block renders offline
  bool inProcess_ = false;   // audio: inside process (MIDI output accepted)
  std::atomic<int> pendingBypass_{-1};
  MidiOutBuffer midiOut_;
  double sampleRate_ = 0;
  int maxBlock_ = 0;
  bool active_ = false;
  std::atomic<int> latency_{0};
  std::vector<float> storage_;
  std::vector<float*> in_, out_;
  std::unique_ptr<Change[]> changes_;
  int pcount_ = 0;
  std::unique_ptr<abi::MidiEvent[]> midi_;
  int ecount_ = 0;
  abi::EventList<kMaxEvents> list_{};
  abi::TimeInfo time_{};
  HostEventQueue events_;
  std::atomic<int32_t> restartFlags_{0};
};

Vst2Editor::~Vst2Editor() {
  if (b_) b_->closeEditor();
}
bool Vst2Editor::size(int& w, int& h) {
  if (!b_) return false;
  abi::Rect* r = nullptr;
  b_->dispatch(abi::opEditorRect, 0, 0, &r);
  if (!r || r->right <= r->left || r->bottom <= r->top) {
    w = 640;
    h = 420;
    return true;
  }
  w = r->right - r->left;
  h = r->bottom - r->top;
  return true;
}
bool Vst2Editor::attach(void* parent) {
  if (!b_ || !parent) return false;
  b_->dispatch(abi::opEditorOpen, 0, 0, parent);
  open = true;
  // Some editors only know their size once open.
  return true;
}
void Vst2Editor::idle() {
  if (b_ && open) b_->dispatch(abi::opEditorIdle);
}
// X11 (J-66): keys typed into the host's window are offered to the editor
// first (macOS editors get theirs from AppKit, see NativeViewMac.mm).
bool Vst2Editor::hostKey(bool down, int usage, uint32_t ch, uint32_t mods) {
  (void)ch;
#if !defined(__APPLE__)
  VstKey k;
  if (!b_ || !open || !vstKeyOf(usage, mods, k)) return false;
  return b_->dispatch(down ? abi::opEditorKeyDown : abi::opEditorKeyUp, static_cast<int32_t>(k.character), k.virtualKey, nullptr,
                      static_cast<float>(k.modifiers)) == 1;
#else
  (void)down, (void)usage, (void)mods;
  return false;
#endif
}

// ---------------------------------------------------------------- format

std::vector<std::string> vst2Paths() {
  std::vector<std::string> v;
  const char* home = gl::platform::homeEnv();
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST");
  v.push_back("/Library/Audio/Plug-Ins/VST");
#elif defined(_WIN32)
  // No standard folder for VST 2: the usual ones of installers and other hosts.
  (void)home;
  for (const char* pf : {"ProgramFiles", "ProgramW6432"})
    for (const char* sub : {"VSTPlugins", "Steinberg/VSTPlugins", "Common Files/VST2", "Common Files/Steinberg/VST2"})
      if (const std::string p = envFolder(pf, sub); !p.empty() && std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
#else
  if (home) v.push_back(std::string(home) + "/.vst");
  addSystemPluginFolders(v, "VST2", "vst");
  v.push_back("/usr/lib/lxvst");
#endif
  addEnvPaths(v, "VST_PATH");
  return v;
}

std::vector<std::string> vst2Discover(const std::vector<std::string>& folders) {
#if defined(__APPLE__)
  return findEntries(folders, ".vst", true, false);
#elif defined(_WIN32)
  return findEntries(folders, ".dll", false, true);
#else
  return findEntries(folders, ".so", false, true);
#endif
}

void describe(Vst2Backend& b, ScannedClass& c, const std::string& path) {
  abi::Effect* fx = b.effect();
  char name[256] = {}, vendor[256] = {}, product[256] = {};
  b.dispatch(abi::opGetName, 0, 0, name);
  b.dispatch(abi::opGetVendor, 0, 0, vendor);
  b.dispatch(abi::opGetProduct, 0, 0, product);
  if (c.name.empty()) c.name = text(name, sizeof name);
  if (c.name.empty()) c.name = text(product, sizeof product);
  if (c.name.empty()) c.name = fs::path(path).stem().string();
  c.vendor = text(vendor, sizeof vendor);
  intptr_t ver = b.dispatch(abi::opGetVendorVersion);
  c.version = ver > 0 ? std::to_string(ver) : std::to_string(fx->pluginVersion);
  c.sdkVersion = "VST " + std::to_string(b.dispatch(abi::opGetApiVersion));
  const intptr_t cat = b.dispatch(abi::opGetCategory);
  c.instrument = (fx->flags & abi::kFlagInstrument) != 0 || cat == abi::catInstrument;
  c.subCategories = c.instrument ? "Instrument" : (cat == abi::catGenerator ? "Generator" : "Fx");
  c.eventInput = b.hasEventInput();
  c.eventOutput = b.hasEventOutput();
  if (b.prepare(48000.0, 512)) {
    c.inputChannels = b.mainInputChannels();
    c.outputChannels = b.mainOutputChannels();
    c.sidechainChannels = b.sidechainChannels();
    c.latency = b.latency();
    for (int k = 0; k < b.auxOutputCount(); ++k) c.auxOutputs.push_back(b.auxOutputName(k));
  }
  for (int k = 0; k < b.programCount(); ++k) c.programs.push_back(b.programName(k));
  for (const ParamInfo& p : b.params()) {
    ScannedParam sp;
    sp.id = p.id;
    sp.title = p.title;
    sp.units = p.units;
    sp.def = p.def;
    sp.flags = p.flags;
    c.params.push_back(std::move(sp));
  }
}

ScanResult vst2Scan(const std::string& path) {
  ScanResult r;
  r.format = "vst2";
  r.bundle = path;
  r.mtime = pathMtime(path);
  std::string err;
  auto b = Vst2Backend::create(path, 0, &err);
  if (!b) {
    r.error = err;
    return r;
  }
  if (b->dispatch(abi::opGetCategory) == abi::catShell) {
    // A shell: one class per sub-plugin.
    std::vector<std::pair<uint32_t, std::string>> subs;
    for (int guard = 0; guard < 4096; ++guard) {
      char name[256] = {};
      intptr_t id = b->dispatch(abi::opShellNext, 0, 0, name);
      if (id == 0) break;
      subs.push_back({static_cast<uint32_t>(id), text(name, sizeof name)});
    }
    for (const auto& [id, name] : subs) {
      std::string perr;
      auto sb = Vst2Backend::create(path, id, &perr);
      if (!sb) {
        r.error += (r.error.empty() ? "" : "; ") + name + ": " + perr;
        continue;
      }
      ScannedClass c;
      c.uid = vst2Type(id).substr(5);
      c.name = name;
      describe(*sb, c, path);
      r.classes.push_back(std::move(c));
      (void)sb.release();  // scanner: never closed
    }
  } else {
    ScannedClass c;
    c.uid = vst2Type(static_cast<uint32_t>(b->effect()->uniqueId)).substr(5);
    describe(*b, c, path);
    r.classes.push_back(std::move(c));
  }
  (void)b.release();
  r.ok = !r.classes.empty();
  if (!r.ok && r.error.empty()) r.error = "no plugins";
  return r;
}

std::unique_ptr<Backend> vst2Create(const std::string& path, const std::string& uid, std::string* err) {
  return Vst2Backend::create(path, fromHex8(uid), err);
}

}  // namespace

Format vst2Format() {
  Format f{};
  f.id = "vst2";
  f.label = "VST2";
  f.defaultPaths = vst2Paths;
  f.discover = vst2Discover;
  f.scanInProcess = vst2Scan;
  f.create = vst2Create;
  f.mtime = pathMtime;
  return f;
}

}  // namespace gl::host

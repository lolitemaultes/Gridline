// Gridline — VST3 as a host::Backend (format "vst3"): wraps vst3::Plugin.
#include <algorithm>
#include <cmath>
#include "../HostedPlugins.h"
#include "../PluginScan.h"
#include "Vst3Editor.h"
#include "Vst3Plugin.h"
#include "Vst3Scan.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

namespace gl::vst3 {

int32_t hostParamFlags(int32_t f);  // Vst3Scan.cpp

namespace {

// IPlugView in a host window.
class View final : public host::Editor {
 public:
  explicit View(sb::IPlugView* v) : view_(v), frame_(new PlugFrame) { frame_->setHeapOwned(); }
  ~View() override {
    if (attached_) view_->removed();
    view_->setFrame(nullptr);
    view_->release();
    frame_->release();
  }
  bool size(int& w, int& h) override {
    sb::ViewRect r{0, 0, 640, 420};
    view_->getSize(&r);
    w = std::max(64, r.getWidth());
    h = std::max(48, r.getHeight());
    return true;
  }
  bool canResize() const override { return view_->canResize() == sb::kResultTrue; }
  void constrain(int& w, int& h) override {
    sb::ViewRect r{0, 0, w, h};
    view_->checkSizeConstraint(&r);
    w = r.getWidth();
    h = r.getHeight();
  }
  bool attach(void* parent) override {
    frame_->onResize = [this](sb::IPlugView* v, sb::ViewRect* nr) {
      if (!requestResize || !requestResize(nr->getWidth(), nr->getHeight())) return false;
      v->onSize(nr);
      return true;
    };
    view_->setFrame(frame_);
    attached_ = parent && view_->attached(parent, platformViewType()) == sb::kResultOk;
    return attached_;
  }
  void setSize(int w, int h) override {
    sb::ViewRect r{0, 0, w, h};
    view_->onSize(&r);
  }
  void setContentScale(double scale) override {
#if !defined(__APPLE__)  // macOS views scale with their window themselves
    void* obj = nullptr;
    if (view_->queryInterface(sb::IPlugViewContentScaleSupport::iid, &obj) == sb::kResultOk && obj) {
      auto* cs = static_cast<sb::IPlugViewContentScaleSupport*>(obj);
      cs->setContentScaleFactor(static_cast<sb::IPlugViewContentScaleSupport::ScaleFactor>(scale));
      cs->release();
    }
#else
    (void)scale;
#endif
  }

 private:
  sb::IPlugView* view_;
  PlugFrame* frame_;
  bool attached_ = false;
};

int32_t restartToHost(int32_t f) {
  int32_t h = 0;
  if (f & (sv::kReloadComponent | sv::kIoChanged)) h |= host::kRestartReload;
  if (f & sv::kLatencyChanged) h |= host::kRestartLatency;
  if (f & sv::kParamValuesChanged) h |= host::kRestartParamValues;
  if (f & (sv::kParamTitlesChanged | sv::kNoteExpressionChanged | sv::kMidiCCAssignmentChanged)) h |= host::kRestartParamInfo;
  return h;
}

class Backend final : public host::Backend {
 public:
  explicit Backend(std::unique_ptr<Plugin> p) : p_(std::move(p)) { readParams(); }

  Plugin* plugin() { return p_.get(); }
  const char* format() const override { return "VST3"; }
  const std::vector<host::ParamInfo>& params() const override { return params_; }
  bool hasEventInput() const override { return p_->hasEventInput(); }
  int mainInputChannels() const override { return p_->mainInputChannels(); }
  int mainOutputChannels() const override { return p_->mainOutputChannels(); }
  int sidechainChannels() const override { return p_->sidechainChannels(); }
  int auxOutputCount() const override { return static_cast<int>(p_->auxOutputBuses().size()); }
  std::string auxOutputName(int k) const override {
    const auto& a = p_->auxOutputBuses();
    std::string n = k >= 0 && static_cast<size_t>(k) < a.size() ? p_->outputBusName(a[static_cast<size_t>(k)]) : std::string();
    return n.empty() ? host::Backend::auxOutputName(k) : n;
  }
  bool supportsTuning() const override { return p_->supportsTuningExpression(); }
  bool hasEventOutput() const override { return p_->hasEventOutput(); }
  bool handlesNotePan() const override { return p_->supportsExpression(sv::kPanTypeID); }
  bool hasBypass() const override { return p_->bypassParam() >= 0; }
  bool setBypass(bool on) override {
    if (p_->bypassParam() < 0) return false;
    p_->setParamNormalized(static_cast<uint32_t>(p_->bypassParam()), on ? 1.0 : 0.0);
    pendingBypass_.store(on ? 1 : 0);
    return true;
  }
  bool setOffline(bool offline) override {
    p_->setOffline(offline);
    return true;
  }
  bool popMidiOut(host::MidiOut& m) override { return p_->midiOut().pop(m); }

  bool prepare(double sr, int mb) override { return p_->prepare(sr, mb); }
  double sampleRate() const override { return p_->sampleRate(); }
  int maxBlock() const override { return p_->maxBlock(); }
  bool active() const override { return p_->active(); }
  void restart(int32_t flags) override {
    int32_t f = 0;
    if (flags & host::kRestartReload) f |= sv::kReloadComponent | sv::kIoChanged;
    if (flags & host::kRestartLatency) f |= sv::kLatencyChanged;
    if (flags & host::kRestartParamInfo) f |= sv::kParamTitlesChanged | sv::kMidiCCAssignmentChanged;
    p_->restart(f);
    if (flags & host::kRestartParamInfo) readParams();
  }
  int latency() const override { return p_->latency(); }
  int tailSamples() const override { return p_->tailSamples(); }
  bool getState(std::string& data) override {
    std::string comp, ctrl;
    if (!p_->getState(comp, ctrl)) return false;
    data = host::encodeState(comp, ctrl);
    return true;
  }
  bool setState(const std::string& data) override {
    std::string comp, ctrl;
    if (!host::decodeState(data, comp, ctrl)) return false;
    return p_->setState(comp, ctrl);
  }
  double paramNormalized(uint32_t id) const override { return p_->paramNormalized(id); }
  void setParamNormalized(uint32_t id, double v) override { p_->setParamNormalized(id, v); }
  std::string paramText(uint32_t id, double n) const override { return p_->paramText(id, n); }
  void drainHostEvents(std::vector<host::HostEvent>& out) override {
    tmp_.clear();
    p_->drainHostEvents(tmp_);
    for (const auto& e : tmp_) {
      host::HostEvent h;
      h.id = e.id;
      h.value = e.value;
      switch (e.kind) {
        case HostEvent::BeginEdit: h.kind = host::HostEvent::BeginEdit; break;
        case HostEvent::PerformEdit: h.kind = host::HostEvent::PerformEdit; break;
        case HostEvent::EndEdit: h.kind = host::HostEvent::EndEdit; break;
        case HostEvent::Restart:
          h.kind = host::HostEvent::Restart;
          h.flags = restartToHost(e.flags);
          break;
        case HostEvent::Dirty: h.kind = host::HostEvent::Dirty; break;
      }
      out.push_back(h);
    }
  }
  bool popOutputValue(host::OutputValue& v) override {
    Plugin::OutputValue o{};
    if (!p_->popOutputValue(o)) return false;
    v.id = o.id;
    v.value = o.value;
    return true;
  }
  int programCount() const override { return static_cast<int>(p_->programs().size()); }
  std::string programName(int i) const override {
    return i >= 0 && static_cast<size_t>(i) < p_->programs().size() ? p_->programs()[static_cast<size_t>(i)] : std::string();
  }
  bool selectProgram(int i) override {
    const int n = programCount();
    if (i < 0 || i >= n || p_->programParam() < 0) return false;
    const double v = n > 1 ? static_cast<double>(i) / (n - 1) : 0.0;
    p_->setParamNormalized(static_cast<uint32_t>(p_->programParam()), v);
    pendingProgram_.store(v);
    return true;
  }
  bool hasEditor() const override { return p_->controller() != nullptr; }
  std::unique_ptr<host::Editor> createEditor() override {
    if (!p_->controller()) return nullptr;
    sb::IPlugView* v = p_->controller()->createView(sv::ViewType::kEditor);
    if (!v) return nullptr;
    if (v->isPlatformTypeSupported(platformViewType()) != sb::kResultTrue) {
      v->release();
      return nullptr;
    }
    return std::make_unique<View>(v);
  }

  float* inputChannel(int ch) override {
    return p_->mainInputBus() >= 0 && ch >= 0 && ch < p_->mainInputChannels() ? p_->inputChannel(p_->mainInputBus(), ch) : nullptr;
  }
  float* sidechainChannel(int ch) override {
    return p_->sidechainBus() >= 0 && ch >= 0 && ch < p_->sidechainChannels() ? p_->inputChannel(p_->sidechainBus(), ch) : nullptr;
  }
  int outputChannels(int bus) const override {
    if (bus == 0) return p_->mainOutputBus() >= 0 ? p_->mainOutputChannels() : 0;
    const auto& a = p_->auxOutputBuses();
    return bus > 0 && static_cast<size_t>(bus) <= a.size() ? p_->outputBusChannels(a[static_cast<size_t>(bus - 1)]) : 0;
  }
  const float* outputChannel(int bus, int ch) override {
    if (ch < 0 || ch >= outputChannels(bus)) return nullptr;
    const int b = bus == 0 ? p_->mainOutputBus() : p_->auxOutputBuses()[static_cast<size_t>(bus - 1)];
    return p_->outputChannel(b, ch);
  }
  bool addParamChange(uint32_t id, double v, int offset) override {
    ParamValueQueue* q = p_->inputParams().queueFor(id);
    if (!q) return false;
    Steinberg::int32 idx = 0;
    q->addPoint(offset, std::clamp(v, 0.0, 1.0), idx);
    return true;
  }
  void addEvent(const host::NoteEvent& n) override {
    sv::Event e{};
    e.busIndex = 0;
    e.sampleOffset = n.offset;
    switch (n.type) {
      case host::NoteEvent::NoteOn:
        e.type = sv::Event::kNoteOnEvent;
        e.noteOn.channel = n.channel;
        e.noteOn.pitch = n.key;
        e.noteOn.tuning = static_cast<float>(n.tuning * 100.0);
        e.noteOn.velocity = std::clamp(n.velocity, 0.0f, 1.0f);
        e.noteOn.length = 0;
        e.noteOn.noteId = n.noteId;
        p_->inputEvents().addEvent(e);
        if (n.hasProps) {  // piano-roll note properties as note expressions
          auto expr = [&](sv::NoteExpressionTypeID type, double v) {
            if (!p_->supportsExpression(type)) return;
            sv::Event x{};
            x.busIndex = 0;
            x.sampleOffset = n.offset;
            x.type = sv::Event::kNoteExpressionValueEvent;
            x.noteExpressionValue.typeId = type;
            x.noteExpressionValue.noteId = n.noteId;
            x.noteExpressionValue.value = std::clamp(v, 0.0, 1.0);
            p_->inputEvents().addEvent(x);
          };
          if (n.pan != 0) expr(sv::kPanTypeID, 0.5 + 0.5 * n.pan);
          if (n.modX != 0.5f) expr(sv::kBrightnessTypeID, n.modX);
        }
        return;
      case host::NoteEvent::NoteOff:
        e.type = sv::Event::kNoteOffEvent;
        e.noteOff.channel = n.channel;
        e.noteOff.pitch = n.key;
        e.noteOff.velocity = n.velocity;
        e.noteOff.noteId = n.noteId;
        e.noteOff.tuning = static_cast<float>(n.tuning * 100.0);
        break;
      case host::NoteEvent::Tuning:
        e.type = sv::Event::kNoteExpressionValueEvent;
        e.noteExpressionValue.typeId = sv::kTuningTypeID;
        e.noteExpressionValue.noteId = n.noteId;
        e.noteExpressionValue.value = std::clamp(0.5 + n.tuning / 240.0, 0.0, 1.0);
        break;
      case host::NoteEvent::Midi: {
        const int st = n.midi[0] & 0xF0, ch = n.midi[0] & 0x0F;
        if (st == 0xA0) {  // polyphonic pressure: an event
          e.type = sv::Event::kPolyPressureEvent;
          e.polyPressure.channel = static_cast<Steinberg::int16>(ch);
          e.polyPressure.pitch = static_cast<Steinberg::int16>(n.midi[1] & 0x7F);
          e.polyPressure.pressure = (n.midi[2] & 0x7F) / 127.0f;
          e.polyPressure.noteId = -1;
          break;
        }
        // Controllers, channel pressure, pitch bend: parameters (IMidiMapping).
        int ctrl = -1;
        double v = 0;
        if (st == 0xB0) ctrl = n.midi[1] & 0x7F, v = (n.midi[2] & 0x7F) / 127.0;
        else if (st == 0xD0) ctrl = sv::kAfterTouch, v = (n.midi[1] & 0x7F) / 127.0;
        else if (st == 0xE0) ctrl = sv::kPitchBend, v = ((n.midi[1] & 0x7F) | ((n.midi[2] & 0x7F) << 7)) / 16383.0;
        const int64_t pid = ctrl >= 0 ? p_->midiParam(ch, ctrl) : -1;
        if (pid >= 0) addParamChange(static_cast<uint32_t>(pid), v, n.offset);
        return;
      }
    }
    p_->inputEvents().addEvent(e);
  }
  void clearInput() override {
    p_->inputParams().clear();
    p_->inputEvents().clear();
  }
  bool process(int n, const host::Transport& t) override {
    double prog = pendingProgram_.exchange(-1.0);
    if (prog >= 0 && p_->programParam() >= 0) addParamChange(static_cast<uint32_t>(p_->programParam()), prog, 0);
    if (const int bp = pendingBypass_.exchange(-1); bp >= 0 && p_->bypassParam() >= 0)
      addParamChange(static_cast<uint32_t>(p_->bypassParam()), bp ? 1.0 : 0.0, 0);
    sv::ProcessContext out{};
    out.state = sv::ProcessContext::kTempoValid | sv::ProcessContext::kTimeSigValid | sv::ProcessContext::kProjectTimeMusicValid |
                sv::ProcessContext::kBarPositionValid | sv::ProcessContext::kContTimeValid | sv::ProcessContext::kCycleValid |
                sv::ProcessContext::kSystemTimeValid | sv::ProcessContext::kClockValid;
    if (t.playing) out.state |= sv::ProcessContext::kPlaying;
    if (t.recording) out.state |= sv::ProcessContext::kRecording;
    if (t.looping) out.state |= sv::ProcessContext::kCycleActive;
    out.cycleStartMusic = t.loopStartQn;
    out.cycleEndMusic = t.loopEndQn;
    out.systemTime = t.systemNanos;
    {  // samples to the next MIDI clock (24 per quarter note)
      const double clockQn = 1.0 / 24.0;
      const double next = std::ceil(t.projectQn / clockQn - 1e-9) * clockQn;
      out.samplesToNextClock = static_cast<Steinberg::int32>(std::lround((next - t.projectQn) * 60.0 / std::max(1.0, t.tempo) * t.sampleRate));
    }
    out.sampleRate = t.sampleRate;
    out.tempo = t.tempo;
    out.timeSigNumerator = t.tsNum;
    out.timeSigDenominator = t.tsDen;
    out.projectTimeMusic = t.projectQn;
    out.barPositionMusic = t.barQn;
    out.projectTimeSamples = t.projectSamples;
    out.continousTimeSamples = t.continuousSamples;
    return p_->process(n, out);
  }

 private:
  void readParams() {
    params_.clear();
    for (const auto& q : p_->params()) {
      host::ParamInfo h;
      h.id = q.id;
      h.title = q.title;
      h.units = q.units;
      h.stepCount = q.stepCount;
      h.def = q.def;
      h.flags = hostParamFlags(q.flags);
      params_.push_back(std::move(h));
    }
    indexParams();
  }
  std::unique_ptr<Plugin> p_;
  std::vector<host::ParamInfo> params_;
  std::vector<HostEvent> tmp_;
  std::atomic<double> pendingProgram_{-1.0};
  std::atomic<int> pendingBypass_{-1};
};

std::unique_ptr<host::Backend> create(const std::string& bundle, const std::string& uid, std::string* err) {
  auto mod = Module::shared(bundle, err);
  if (!mod) return nullptr;
  auto p = Plugin::create(mod, uid, err);
  if (!p) return nullptr;
  return std::make_unique<Backend>(std::move(p));
}

}  // namespace

Plugin* pluginOf(host::Backend* b) {
  auto* v = dynamic_cast<Backend*>(b);
  return v ? v->plugin() : nullptr;
}

}  // namespace gl::vst3

namespace gl::host {
Format vst3Format() {
  Format f{};
  f.id = "vst3";
  f.label = "VST3";
  f.defaultPaths = [] { return vst3::defaultSearchPaths(); };
  f.discover = [](const std::vector<std::string>& folders) { return vst3::findBundles(folders); };
  f.scanInProcess = vst3::scanBundleInProcess;
  f.create = vst3::create;
  f.mtime = vst3::bundleMtime;
  return f;
}
}  // namespace gl::host

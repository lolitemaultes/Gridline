// Gridline yabridge test: the probe as a Windows VST3 (one instrument and one effect class),
// written against the SDK interface headers only. See probe_core.h.
#include "probe_core.h"
#include <new>
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
DEF_CLASS_IID(IPlugViewContentScaleSupport)
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IEventList)
}  // namespace Vst
}  // namespace Steinberg

namespace {

const FUID kSynthUid(0x47525042, 0x53594E54, 0x48565354, 0x33000001);
const FUID kFxUid(0x47525042, 0x46580000, 0x00565354, 0x33000002);

bool same(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }
void setString(String128 dst, const char* src) {
  size_t i = 0;
  for (; src[i] && i < 127; ++i) dst[i] = static_cast<TChar>(src[i]);
  dst[i] = 0;
}

class Plugin;

class View final : public IPlugView, public IPlugViewContentScaleSupport, public probe::Host {
 public:
  explicit View(Plugin* owner);
  ~View() override;
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same(iid, IPlugView::iid) || same(iid, FUnknown::iid)) {
      *obj = static_cast<IPlugView*>(this);
    } else if (same(iid, IPlugViewContentScaleSupport::iid)) {
      *obj = static_cast<IPlugViewContentScaleSupport*>(this);
    } else {
      *obj = nullptr;
      return kNoInterface;
    }
    addRef();
    return kResultOk;
  }
  uint32 PLUGIN_API addRef() override { return ++refs_; }
  uint32 PLUGIN_API release() override {
    const uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse; }
  tresult PLUGIN_API attached(void* parent, FIDString type) override;
  tresult PLUGIN_API removed() override {
    window_.close();
    return kResultOk;
  }
  tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
  tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API getSize(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    *r = ViewRect(0, 0, window_.width(), window_.height());
    return kResultOk;
  }
  tresult PLUGIN_API onSize(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    window_.resize(r->getWidth(), r->getHeight());
    return kResultOk;
  }
  tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
  tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
    frame_ = frame;
    return kResultOk;
  }
  tresult PLUGIN_API canResize() override { return kResultTrue; }
  tresult PLUGIN_API checkSizeConstraint(ViewRect*) override { return kResultOk; }
  tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override;

  void beginEdit(int param) override;
  void performEdit(int param, double value) override;
  void endEdit(int param) override;
  bool requestSize(int w, int h) override {
    if (!frame_) return false;
    ViewRect r(0, 0, w, h);
    return frame_->resizeView(this, &r) == kResultOk;
  }

 private:
  std::atomic<uint32> refs_{1};
  Plugin* owner_;
  probe::Window window_;
  IPlugFrame* frame_ = nullptr;
};

class Plugin final : public IComponent, public IAudioProcessor, public IEditController {
 public:
  explicit Plugin(bool synth) : core(synth) {}
  ~Plugin() { probe::logf(core.inst, "destroy"); }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same(iid, IComponent::iid)) *obj = static_cast<IComponent*>(this);
    else if (same(iid, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
    else if (same(iid, IEditController::iid)) *obj = static_cast<IEditController*>(this);
    else if (same(iid, IPluginBase::iid) || same(iid, FUnknown::iid)) *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
    else {
      *obj = nullptr;
      return kNoInterface;
    }
    addRef();
    return kResultOk;
  }
  uint32 PLUGIN_API addRef() override { return ++refs_; }
  uint32 PLUGIN_API release() override {
    const uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
  tresult PLUGIN_API terminate() override { return kResultOk; }
  tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type == kEvent) return core.synth && dir == kInput ? 1 : 0;
    return dir == kOutput || !core.synth ? 1 : 0;
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (index != 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus.mediaType = type;
    bus.direction = dir;
    bus.channelCount = type == kAudio ? 2 : 16;
    setString(bus.name, type == kAudio ? "Main" : "Notes");
    bus.busType = kMain;
    bus.flags = BusInfo::kDefaultActive;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
  tresult PLUGIN_API setActive(TBool on) override {
    probe::logf(core.inst, "active %d", on ? 1 : 0);
    if (!on) core.logAudio();
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    char buf[probe::Core::kStateSize];
    int32 got = 0;
    if (!s || s->read(buf, sizeof buf, &got) != kResultOk) return kResultFalse;
    return core.loadState(buf, static_cast<size_t>(got)) ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    char buf[probe::Core::kStateSize];
    core.saveState(buf);
    return s && s->write(buf, sizeof buf, nullptr) == kResultOk ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 numIns, SpeakerArrangement* out, int32 numOuts) override {
    const bool ok = numOuts == 1 && out[0] == SpeakerArr::kStereo && (core.synth ? numIns == 0 : numIns == 1 && in[0] == SpeakerArr::kStereo);
    return ok ? kResultTrue : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement& arr) override {
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return core.synth ? 0 : probe::kLatency; }
  tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
    core.sr = setup.sampleRate;
    probe::logf(core.inst, "setup rate %.0f block %d mode %d", setup.sampleRate, setup.maxSamplesPerBlock, setup.processMode);
    return kResultOk;
  }
  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
  uint32 PLUGIN_API getTailSamples() override { return 0; }
  tresult PLUGIN_API process(ProcessData& d) override {
    if (IParameterChanges* pc = d.inputParameterChanges)
      for (int32 i = 0; i < pc->getParameterCount(); ++i) {
        IParamValueQueue* q = pc->getParameterData(i);
        if (!q || q->getPointCount() <= 0 || q->getParameterId() >= probe::kParams) continue;
        int32 off = 0;
        ParamValue v = 0;
        if (q->getPoint(q->getPointCount() - 1, off, v) == kResultOk) core.p[q->getParameterId()].store(v);
      }
    if (IEventList* ev = d.inputEvents)
      for (int32 i = 0; i < ev->getEventCount(); ++i) {
        Event e{};
        if (ev->getEvent(i, e) != kResultOk) continue;
        if (e.type == Event::kNoteOnEvent) core.noteOn(e.noteOn.pitch, static_cast<double>(e.noteOn.velocity));
        if (e.type == Event::kNoteOffEvent) core.noteOff(e.noteOff.pitch);
      }
    if (d.numOutputs < 1 || d.numSamples <= 0) return kResultOk;
    const float* const* in = !core.synth && d.numInputs > 0 ? d.inputs[0].channelBuffers32 : nullptr;
    core.render(in, d.outputs[0].channelBuffers32, d.numSamples);
    return kResultOk;
  }
  // IEditController
  tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
  int32 PLUGIN_API getParameterCount() override { return probe::kParams; }
  tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
    if (index < 0 || index >= probe::kParams) return kInvalidArgument;
    info = ParameterInfo{};
    info.id = static_cast<ParamID>(index);
    info.flags = ParameterInfo::kCanAutomate;
    info.stepCount = index == 2 ? 1 : 0;
    info.defaultNormalizedValue = probe::kParamDefaults[index];
    setString(info.title, probe::kParamNames[index]);
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue v, String128 s) override {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    setString(s, buf);
    return kResultOk;
  }
  tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id < probe::kParams ? core.p[id].load() : 0.0; }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
    if (id >= probe::kParams) return kInvalidArgument;
    core.p[id].store(v);
    probe::logf(core.inst, "hostset %u %.6f", static_cast<unsigned>(id), v);
    return kResultOk;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
    handler = h;
    return kResultOk;
  }
  IPlugView* PLUGIN_API createView(FIDString name) override {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;
    return new (std::nothrow) View(this);
  }

  probe::Core core;
  IComponentHandler* handler = nullptr;

 private:
  std::atomic<uint32> refs_{1};
};

View::View(Plugin* owner) : owner_(owner), window_(owner->core, *this) { owner_->addRef(); }
View::~View() {
  window_.close();
  owner_->release();
}
tresult PLUGIN_API View::attached(void* parent, FIDString type) {
  if (isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
  return window_.open(static_cast<HWND>(parent), window_.width(), window_.height()) ? kResultOk : kResultFalse;
}
tresult PLUGIN_API View::setContentScaleFactor(ScaleFactor factor) {
  probe::logf(owner_->core.inst, "scale %.3f", static_cast<double>(factor));
  return kResultOk;
}
void View::beginEdit(int param) {
  if (owner_->handler) owner_->handler->beginEdit(static_cast<ParamID>(param));
}
void View::performEdit(int param, double value) {
  if (owner_->handler) owner_->handler->performEdit(static_cast<ParamID>(param), value);
}
void View::endEdit(int param) {
  if (owner_->handler) owner_->handler->endEdit(static_cast<ParamID>(param));
}

class Factory final : public IPluginFactory {
 public:
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same(iid, IPluginFactory::iid) || same(iid, FUnknown::iid)) {
      *obj = static_cast<IPluginFactory*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
    *info = PFactoryInfo{};
    std::strcpy(info->vendor, "Gridline Tests");
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 2; }
  tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
    if (index < 0 || index > 1) return kInvalidArgument;
    *info = PClassInfo{};
    std::memcpy(info->cid, (index == 0 ? kSynthUid : kFxUid).toTUID(), sizeof(TUID));
    info->cardinality = PClassInfo::kManyInstances;
    std::strcpy(info->category, kVstAudioEffectClass);
    std::strcpy(info->name, index == 0 ? "Gridline Probe Synth" : "Gridline Probe Fx");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
    *obj = nullptr;
    Plugin* inst = nullptr;
    if (same(cid, kSynthUid.toTUID())) inst = new (std::nothrow) Plugin(true);
    else if (same(cid, kFxUid.toTUID())) inst = new (std::nothrow) Plugin(false);
    if (!inst) return kNoInterface;
    const tresult r = inst->queryInterface(iid, obj);
    inst->release();
    return r;
  }
};

}  // namespace

extern "C" {
__declspec(dllexport) IPluginFactory* PLUGIN_API GetPluginFactory() {
  static Factory f;
  return &f;
}
__declspec(dllexport) bool PLUGIN_API InitDll() { return true; }
__declspec(dllexport) bool PLUGIN_API ExitDll() { return true; }
}

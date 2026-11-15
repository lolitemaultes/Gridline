// Gridline — a strict VST3 instrument for the host tests (built in-tree as
// GridlineStrict.vst3 in its own folder; never installed). Written against the
// SDK interface headers only. It behaves like hardware emulations and other
// picky instruments do:
//   - no audio input, three stereo output buses, the extra ones switched off
//     by default;
//   - the main output on bus 0 ("Gridline Strict Synth") or on bus 1, between
//     two extra outputs ("Gridline Strict Synth Mid");
//   - a host that calls it out of order (bus layout or setup while active,
//     process before setProcessing, too long blocks) or passes buffers that do
//     not match the active buses gets silence, and the probe says why;
//   - silent for its first kWarmup frames after processing starts (an
//     emulation booting) and 64 samples of reported latency;
//   - the notes of an event bus the host left switched off are ignored.
#include <algorithm>
#include <atomic>
#include <cmath>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <cstring>
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "../common/StrictProbe.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Steinberg {
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
}  // namespace Vst
}  // namespace Steinberg

namespace {

GridlineStrictProbe gProbe;
constexpr int kBuses = 3;
constexpr int kLatency = 64;
constexpr int64_t kWarmup = 2048;

const FUID kStrictUid(0x47524944, 0x4C494E45, 0x53545231, 0x00000001);
const FUID kStrictMidUid(0x47524944, 0x4C494E45, 0x53545231, 0x00000002);

bool same(const TUID a, const FUID& b) { return std::memcmp(a, b.toTUID(), sizeof(TUID)) == 0; }
void setString(String128 dst, const char* src) {
  size_t i = 0;
  for (; src[i] && i < 127; ++i) dst[i] = static_cast<TChar>(src[i]);
  dst[i] = 0;
}

class StrictSynth final : public IComponent, public IAudioProcessor, public IEditController {
 public:
  explicit StrictSynth(int mainBus) : main_(mainBus) {
    gProbe.mainBus = main_;  // every bus starts switched off (as in the SDK): the host switches on what it uses
  }
  ~StrictSynth() = default;  // (final: not virtual)
  uint32 PLUGIN_API addRef() override { return ++refs_; }
  uint32 PLUGIN_API release() override {
    const uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same(iid, IComponent::iid) || same(iid, IPluginBase::iid) || same(iid, FUnknown::iid)) *obj = static_cast<IComponent*>(this);
    else if (same(iid, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
    else if (same(iid, IEditController::iid)) *obj = static_cast<IEditController*>(this);
    else {
      *obj = nullptr;
      return kNoInterface;
    }
    addRef();
    return kResultOk;
  }
  tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
  tresult PLUGIN_API terminate() override { return kResultOk; }
  // IComponent
  tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type == kAudio) return dir == kOutput ? kBuses : 0;
    return dir == kInput ? 1 : 0;
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus.mediaType = type;
    bus.direction = dir;
    if (type == kEvent) {
      bus.channelCount = 16;
      setString(bus.name, "MIDI In");
      bus.busType = kMain;
      bus.flags = BusInfo::kDefaultActive;
      return kResultOk;
    }
    bus.channelCount = 2;
    static const char* const kNames[2][kBuses] = {{"Main Out", "Out 3+4", "Out 5+6"}, {"Out 1+2", "Main Out", "Out 5+6"}};
    setString(bus.name, kNames[main_ == 0 ? 0 : 1][index]);
    bus.busType = index == main_ ? kMain : kAux;
    bus.flags = index == main_ ? BusInfo::kDefaultActive : 0;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override {
    if (active_) gProbe.violations |= kStrictBusWhileActive;
    if (type == kEvent && dir == kInput && index == 0) events_ = state != 0;
    else if (type == kAudio && dir == kOutput && index >= 0 && index < kBuses) on_[index] = state != 0;
    else return kInvalidArgument;
    return kResultOk;
  }
  tresult PLUGIN_API setActive(TBool state) override {
    if (state && sampleRate_ <= 0) gProbe.violations |= kStrictActiveWithoutSetup;
    if (!state && processing_) gProbe.violations |= kStrictDeactivateProcessing;
    active_ = state != 0;
    if (active_) amp_ = 0, frames_ = 0;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    int32 magic = 0, got = 0;
    return s && s->read(&magic, sizeof magic, &got) == kResultOk && got == sizeof magic && magic == 0x53545231 ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    if (!s) return kInvalidArgument;
    int32 magic = 0x53545231;
    s->write(&magic, sizeof magic, nullptr);
    return kResultOk;
  }
  // IAudioProcessor: stereo on every output, nothing else.
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32 numIns, SpeakerArrangement* outs, int32 numOuts) override {
    if (active_) gProbe.violations |= kStrictArrangeWhileActive;
    if (numIns != 0 || numOuts != kBuses) return kResultFalse;
    for (int32 i = 0; i < numOuts; ++i)
      if (outs[i] != SpeakerArr::kStereo) return kResultFalse;
    return kResultTrue;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
    if (dir != kOutput || index < 0 || index >= kBuses) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return kLatency; }
  tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
    if (active_) gProbe.violations |= kStrictSetupWhileActive;
    sampleRate_ = setup.sampleRate;
    maxBlock_ = setup.maxSamplesPerBlock;
    gProbe.sampleRate = sampleRate_;
    gProbe.setupMaxBlock = maxBlock_;
    return sampleRate_ > 0 && maxBlock_ > 0 ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API setProcessing(TBool state) override {
    if (state && !active_) gProbe.violations |= kStrictProcessInactive;
    processing_ = state != 0;
    if (processing_) ++gProbe.starts;
    return kResultOk;
  }
  uint32 PLUGIN_API getTailSamples() override { return 0; }
  tresult PLUGIN_API process(ProcessData& d) override {
    bool ok = true;
    auto fail = [&](uint32_t bit) {
      gProbe.violations |= bit;
      ok = false;
    };
    if (!active_) fail(kStrictProcessInactive);
    if (!processing_) fail(kStrictProcessNotStarted);
    if (d.numSamples > maxBlock_) fail(kStrictBlockTooLong);
    if (d.numInputs != 0) fail(kStrictInputs);
    // Every active bus must come with its two channels; buses the host leaves out must be off.
    int activeOuts = 0;
    for (int b = 0; b < kBuses; ++b) {
      if (!on_[b]) continue;
      ++activeOuts;
      if (b >= d.numOutputs || !d.outputs) {
        fail(kStrictLayout);
        continue;
      }
      const AudioBusBuffers& bus = d.outputs[b];
      if (bus.numChannels != 2 || !bus.channelBuffers32 || !bus.channelBuffers32[0] || !bus.channelBuffers32[1]) fail(kStrictLayout);
    }
    if (d.numOutputs > kBuses) fail(kStrictLayout);
    gProbe.activeOutputs = activeOuts;
    if (d.numSamples <= 0) {  // a parameter flush: nothing to render
      ++gProbe.zeroBlocks;
      return kResultOk;
    }
    ++gProbe.blocks;
    gProbe.minBlock = gProbe.minBlock == 0 ? d.numSamples : std::min(gProbe.minBlock, d.numSamples);
    gProbe.maxBlock = std::max(gProbe.maxBlock, d.numSamples);
    IEventList* ev = d.inputEvents;
    const int32 evCount = ev ? ev->getEventCount() : 0;
    if (evCount > 0 && !events_) gProbe.violations |= kStrictEventsInactive;
    int32 ei = 0;
    Event e{};
    for (int32 i = 0; i < d.numSamples; ++i) {
      while (events_ && ei < evCount && ev->getEvent(ei, e) == kResultOk && e.sampleOffset <= i) {
        ++ei;
        if (e.type == Event::kNoteOnEvent) {
          freq_ = 440.0 * std::pow(2.0, (e.noteOn.pitch - 69.0) / 12.0);
          startAt_ = frames_ + kLatency;
          stopAt_ = -1;
          ++gProbe.notesOn;
        } else if (e.type == Event::kNoteOffEvent) {
          stopAt_ = frames_ + kLatency;
        }
      }
      const bool ready = ok && frames_ >= kWarmup;
      if (startAt_ >= 0 && frames_ >= startAt_) amp_ = 0.5, startAt_ = -1, phase_ = 0;
      if (stopAt_ >= 0 && frames_ >= stopAt_) amp_ = 0, stopAt_ = -1;
      float s = 0;
      if (ready && amp_ > 0) {
        phase_ += freq_ / sampleRate_;
        phase_ -= std::floor(phase_);
        s = static_cast<float>(std::sin(6.283185307179586 * phase_) * amp_);
      }
      for (int b = 0; b < kBuses && b < d.numOutputs; ++b) {
        const AudioBusBuffers& bus = d.outputs[b];
        if (!on_[b] || bus.numChannels < 2 || !bus.channelBuffers32 || !bus.channelBuffers32[0] || !bus.channelBuffers32[1]) continue;
        // Main: the voice; the extra outputs: the same at half and a quarter level.
        const int k = b == main_ ? 0 : b < main_ ? b + 1 : b;
        const float x = s / static_cast<float>(1 << k);
        bus.channelBuffers32[0][i] = bus.channelBuffers32[1][i] = x;
      }
      ++frames_;
    }
    gProbe.frames = frames_;
    return kResultOk;
  }
  // IEditController: no parameters.
  tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
  int32 PLUGIN_API getParameterCount() override { return 0; }
  tresult PLUGIN_API getParameterInfo(int32, ParameterInfo&) override { return kInvalidArgument; }
  tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kInvalidArgument; }
  tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API getParamNormalized(ParamID) override { return 0; }
  tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
  tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
  IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

 private:
  std::atomic<uint32> refs_{1};
  int main_ = 0;
  bool on_[kBuses] = {};
  bool events_ = false;  // the event bus starts switched off: the host must switch it on
  bool active_ = false, processing_ = false;
  double sampleRate_ = 0;
  int32 maxBlock_ = 0;
  int64_t frames_ = 0, startAt_ = -1, stopAt_ = -1;
  double freq_ = 440, amp_ = 0, phase_ = 0;
};

class Factory final : public IPluginFactory2 {
 public:
  ~Factory() = default;
  uint32 PLUGIN_API addRef() override { return ++refs_; }
  uint32 PLUGIN_API release() override {
    const uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same(iid, IPluginFactory2::iid) || same(iid, IPluginFactory::iid) || same(iid, FUnknown::iid)) {
      *obj = static_cast<IPluginFactory2*>(this);
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
    *info = PFactoryInfo{};
    std::strcpy(info->vendor, "Gridline Tests");
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 2; }
  tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
    PClassInfo2 i2{};
    if (getClassInfo2(index, &i2) != kResultOk) return kInvalidArgument;
    *info = PClassInfo{};
    std::memcpy(info->cid, i2.cid, sizeof(TUID));
    info->cardinality = i2.cardinality;
    std::strcpy(info->category, i2.category);
    std::strcpy(info->name, i2.name);
    return kResultOk;
  }
  tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
    if (index < 0 || index > 1) return kInvalidArgument;
    *info = PClassInfo2{};
    std::memcpy(info->cid, (index == 0 ? kStrictUid : kStrictMidUid).toTUID(), sizeof(TUID));
    info->cardinality = PClassInfo::kManyInstances;
    std::strcpy(info->category, kVstAudioEffectClass);
    std::strcpy(info->name, index == 0 ? "Gridline Strict Synth" : "Gridline Strict Synth Mid");
    std::strcpy(info->subCategories, "Instrument|Synth");
    std::strcpy(info->vendor, "Gridline Tests");
    std::strcpy(info->version, "1.0.0");
    std::strcpy(info->sdkVersion, "VST 3.8.1");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
    *obj = nullptr;
    StrictSynth* inst = same(cid, kStrictUid) ? new StrictSynth(0) : same(cid, kStrictMidUid) ? new StrictSynth(1) : nullptr;
    if (!inst) return kNoInterface;
    const tresult r = inst->queryInterface(iid, obj);
    inst->release();
    return r;
  }

 private:
  std::atomic<uint32> refs_{1};
};

}  // namespace

extern "C" {
SMTG_EXPORT_SYMBOL GridlineStrictProbe* gridline_strict_probe() { return &gProbe; }
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return new Factory; }
#if defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif
}

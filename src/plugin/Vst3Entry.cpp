// Gridline — VST 3 entry point of Gridline as a plugin (PH-09). Written
// against the VST 3 interface headers only (pluginterfaces, MIT); no SDK
// helper classes. Two audio classes, each a single component (processor and
// controller in one object):
//   "Gridline"     instrument: note events in, stereo out + 15 stereo
//                  outputs (mixer inserts 1-15)
//   "Gridline FX"  effect: stereo in (mixer insert 1) + sidechain, stereo out
// Parameters are the 16 macros; the state is the whole project; the editor
// is Gridline's interface (NSView / X11 child window), resizable.
//
// J-131: built with GRIDLINE_SUITE="<suite id>", the same entry point is a
// Gridline Suite bundle: one class per member of the suite (core/PluginSuite.h),
// each one built-in instrument or effect with its own parameters (effects add
// Mix and Bypass), state and editor. Instruments: notes in, stereo out.
// Effects: stereo in (+ a sidechain bus for the effects that read one), stereo
// out (+ notes in for MIDI effects).
//
// MIDI controllers: VST 3 delivers notes as events but controllers, channel
// pressure and pitch bend only as parameters. IMidiMapping assigns each of
// them, per MIDI channel, to a hidden parameter (16 x 130: CC 0-127, channel
// pressure, pitch bend); their points are turned back into MIDI messages at
// their sample offsets, so Gridline receives the same MIDI as from CLAP.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "PluginCore.h"
#include "PluginView.h"
#ifdef GRIDLINE_SUITE
#include "../core/PluginSuite.h"
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;
using GlEvent = gl::plugin::Event;
using gl::plugin::Instance;
using gl::plugin::kExtraOutputs;
using gl::plugin::kMacros;
using gl::plugin::Variant;

namespace {

const FUID kInstrumentUid(0x47524944, 0x4C494E45, 0x504C5547, 0x00000001);
const FUID kEffectUid(0x47524944, 0x4C494E45, 0x504C5547, 0x00000002);
constexpr int kMaxEvents = 4096;

// Hidden MIDI controller parameters: id = kMidiBase + channel * kMidiCtrls + controller
// (0-127 CC, 128 channel pressure, 129 pitch bend: the VST 3 controller numbers).
constexpr ParamID kMidiBase = 0x10000;
constexpr int kMidiCtrls = 130;
constexpr int kMidiParams = 16 * kMidiCtrls;
static_assert(kAfterTouch == 128 && kPitchBend == 129, "VST 3 controller numbers");

bool isMidiParam(ParamID id) { return id >= kMidiBase && id < kMidiBase + static_cast<ParamID>(kMidiParams); }

// A MIDI controller parameter's value as a MIDI message (false: not one).
bool midiFromParam(ParamID id, double v, uint8_t out[3]) {
  if (!isMidiParam(id)) return false;
  const int k = static_cast<int>(id - kMidiBase), ch = k / kMidiCtrls, ctrl = k % kMidiCtrls;
  v = std::clamp(v, 0.0, 1.0);
  if (ctrl < 128) {
    out[0] = static_cast<uint8_t>(0xB0 | ch);
    out[1] = static_cast<uint8_t>(ctrl);
    out[2] = static_cast<uint8_t>(std::lround(v * 127.0));
  } else if (ctrl == kAfterTouch) {
    out[0] = static_cast<uint8_t>(0xD0 | ch);
    out[1] = static_cast<uint8_t>(std::lround(v * 127.0));
    out[2] = 0;
  } else {
    const int raw = static_cast<int>(std::lround(v * 16383.0));
    out[0] = static_cast<uint8_t>(0xE0 | ch);
    out[1] = static_cast<uint8_t>(raw & 0x7F);
    out[2] = static_cast<uint8_t>((raw >> 7) & 0x7F);
  }
  return true;
}

bool same(const TUID a, const FUID& b) { return std::memcmp(a, b.toTUID(), sizeof(TUID)) == 0; }
bool sameIid(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

void setString(String128 dst, const std::string& src) {
  size_t i = 0;
  for (; i < src.size() && i < 127; ++i) dst[i] = static_cast<TChar>(static_cast<unsigned char>(src[i]));
  dst[i] = 0;
}

std::string narrow(const TChar* s) {
  std::string out;
  for (; s && *s; ++s) out.push_back(*s < 128 ? static_cast<char>(*s) : '?');
  return out;
}

double seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class RefCounted {
 public:
  virtual ~RefCounted() = default;

 protected:
  uint32 add() { return ++refs_; }
  uint32 rel() {
    const uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }

 private:
  std::atomic<uint32> refs_{1};
};

#define GRIDLINE_REFCOUNT                                \
  uint32 PLUGIN_API addRef() override { return add(); } \
  uint32 PLUGIN_API release() override { return rel(); }

#define GRIDLINE_QI(I)              \
  if (sameIid(iid, I::iid)) {       \
    *obj = static_cast<I*>(this);   \
    addRef();                       \
    return kResultOk;               \
  }

// ---------------------------------------------------------------- editor

class GridlineView final : public RefCounted, public IPlugView, public IPlugViewContentScaleSupport, public Linux::ITimerHandler {
 public:
  explicit GridlineView(Instance& inst) : inst_(inst) { inst.preferredViewSize(w_, h_); }
  ~GridlineView() override { removed(); }
  GRIDLINE_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    GRIDLINE_QI(IPlugView)
    GRIDLINE_QI(IPlugViewContentScaleSupport)
    GRIDLINE_QI(Linux::ITimerHandler)
    if (sameIid(iid, FUnknown::iid)) {
      *obj = static_cast<IPlugView*>(this);
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
    if (!type || !gl::plugin::viewSupported(gl::plugin::nativeWindowApi())) return kResultFalse;
    const gl::plugin::WindowApi api = gl::plugin::nativeWindowApi();
    const char* want = api == gl::plugin::WindowApi::Cocoa   ? kPlatformTypeNSView
                       : api == gl::plugin::WindowApi::Win32 ? kPlatformTypeHWND
                                                             : kPlatformTypeX11EmbedWindowID;
    return std::strcmp(type, want) == 0 ? kResultTrue : kResultFalse;
  }
  tresult PLUGIN_API attached(void* parent, FIDString type) override {
    if (isPlatformTypeSupported(type) != kResultTrue || !parent || view_) return kResultFalse;
    view_ = gl::plugin::createView(inst_);
    if (!view_) return kResultFalse;
    view_->setSize(w_, h_);
    if (scale_ > 0) view_->setScale(scale_);
    view_->requestResize = [this](int w, int h) {
      if (!frame_) return false;
      ViewRect r(0, 0, w, h);
      return frame_->resizeView(this, &r) == kResultTrue;
    };
    if (!view_->attach(parent)) {
      view_.reset();
      return kResultFalse;
    }
    if (frame_ && gl::plugin::nativeWindowApi() == gl::plugin::WindowApi::X11) {
      if (frame_->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&runLoop_)) == kResultOk && runLoop_)
        runLoop_->registerTimer(this, 16);
    }
    return kResultOk;
  }
  tresult PLUGIN_API removed() override {
    if (runLoop_) {
      runLoop_->unregisterTimer(this);
      runLoop_->release();
      runLoop_ = nullptr;
    }
    if (view_) view_->detach();
    view_.reset();
    return kResultOk;
  }
  tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
  tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API getSize(ViewRect* size) override {
    if (!size) return kInvalidArgument;
    if (view_) view_->size(w_, h_);
    *size = ViewRect(0, 0, w_, h_);
    return kResultOk;
  }
  tresult PLUGIN_API onSize(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    int minW = 0, minH = 0;
    inst_.minViewSize(minW, minH);
    w_ = std::max<int>(r->getWidth(), minW);
    h_ = std::max<int>(r->getHeight(), minH);
    if (view_) view_->setSize(w_, h_);
    return kResultOk;
  }
  tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
  tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
    frame_ = frame;
    return kResultOk;
  }
  tresult PLUGIN_API canResize() override { return kResultTrue; }
  tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    int minW = 0, minH = 0, cw = 0, ch = 0;
    inst_.minViewSize(minW, minH);
    inst_.editorContent(cw, ch);
    if (inst_.single() && cw > 0 && ch > 0) {  // a suite editor zooms at its own proportions
      const double z = std::clamp(std::min(static_cast<double>(r->getWidth()) / cw, static_cast<double>(r->getHeight()) / ch), 0.5, 3.0);
      r->right = r->left + static_cast<int32>(std::lround(cw * z));
      r->bottom = r->top + static_cast<int32>(std::lround(ch * z));
    }
    if (r->getWidth() < minW) r->right = r->left + minW;
    if (r->getHeight() < minH) r->bottom = r->top + minH;
    return kResultTrue;
  }
  tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override {
    scale_ = factor;
    if (view_) view_->setScale(factor);
    return kResultOk;
  }
  void PLUGIN_API onTimer() override {
    if (view_) view_->tick();
    inst_.idle();
  }

 private:
  Instance& inst_;
  std::unique_ptr<gl::plugin::View> view_;
  IPlugFrame* frame_ = nullptr;
  Linux::IRunLoop* runLoop_ = nullptr;
  int w_ = gl::plugin::kDefaultViewW, h_ = gl::plugin::kDefaultViewH;
  double scale_ = 0;
};

// ---------------------------------------------------------------- component

class Component final : public RefCounted, public IComponent, public IAudioProcessor, public IEditController, public IMidiMapping {
 public:
  // `type`: a Gridline Suite member ("" = the Gridline workstation).
  Component(Variant v, std::string type, bool sidechain, bool midiInput)
      : variant_(v), type_(std::move(type)), sidechain_(sidechain), midiInput_(midiInput) {
    for (auto& m : midiValues_) m.store(0.f, std::memory_order_relaxed);
    for (int ch = 0; ch < 16; ++ch) midiValues_[ch * kMidiCtrls + kPitchBend].store(0.5f, std::memory_order_relaxed);
  }
  ~Component() override { terminate(); }
  GRIDLINE_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    GRIDLINE_QI(IComponent)
    GRIDLINE_QI(IAudioProcessor)
    GRIDLINE_QI(IEditController)
    GRIDLINE_QI(IMidiMapping)
    if (sameIid(iid, IPluginBase::iid) || sameIid(iid, FUnknown::iid)) {
      *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  // ---- IPluginBase (shared by IComponent and IEditController) ----
  tresult PLUGIN_API initialize(FUnknown*) override {
    if (inst_) return kResultOk;
    inst_ = type_.empty() ? std::make_unique<Instance>(variant_) : std::make_unique<Instance>(variant_, type_);
    if (!type_.empty() && !inst_->single()) {  // not a plugin of this build
      inst_.reset();
      return kResultFalse;
    }
    inst_->onLatencyChanged = [this]() {
      // During setActive the host reads getLatencySamples() afterwards anyway.
      if (handler_ && !inSetActive_) handler_->restartComponent(kLatencyChanged);
    };
    inst_->onMacrosChanged = [this]() {
      if (handler_) handler_->restartComponent(kParamValuesChanged);
    };
    if (inst_->single()) {  // J-131: edits come inside their gestures
      inst_->onMacroEdited = [this](int k, double v) {
        if (handler_) handler_->performEdit(inst_->paramId(k), v);
      };
      inst_->onParamGesture = [this](int k, bool begin) {
        if (!handler_) return;
        if (begin)
          handler_->beginEdit(inst_->paramId(k));
        else
          handler_->endEdit(inst_->paramId(k));
      };
    } else {
      inst_->onMacroEdited = [this](int k, double v) {
        if (!handler_) return;
        handler_->beginEdit(static_cast<ParamID>(k));
        handler_->performEdit(static_cast<ParamID>(k), v);
        handler_->endEdit(static_cast<ParamID>(k));
      };
    }
#if defined(__APPLE__)
    // Main-thread housekeeping while no editor is open (the run loop of the host's main thread).
    CFRunLoopTimerContext ctx{};
    ctx.info = this;
    timer_ = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.05, 0.03, 0, 0,
                                  [](CFRunLoopTimerRef, void* info) { static_cast<Component*>(info)->idle(); }, &ctx);
    if (timer_) CFRunLoopAddTimer(CFRunLoopGetMain(), timer_, kCFRunLoopCommonModes);
#endif
    return kResultOk;
  }
  tresult PLUGIN_API terminate() override {
#if defined(__APPLE__)
    if (timer_) {
      CFRunLoopTimerInvalidate(timer_);
      CFRelease(timer_);
      timer_ = nullptr;
    }
#endif
    inst_.reset();
    return kResultOk;
  }

  // ---- IComponent ----
  tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }  // single component
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type == kEvent) return dir == kInput && takesNotes() ? 1 : 0;
    if (variant_ == Variant::Effect) return dir == kInput ? (sidechain_ ? 2 : 1) : 1;
    return dir == kInput ? 0 : (type_.empty() ? 1 + kExtraOutputs : 1);
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus = BusInfo{};
    bus.mediaType = type;
    bus.direction = dir;
    if (type == kEvent) {
      bus.channelCount = 16;
      setString(bus.name, "Notes");
      bus.busType = kMain;
      bus.flags = BusInfo::kDefaultActive;
      return kResultOk;
    }
    bus.channelCount = 2;
    bus.busType = index == 0 ? kMain : kAux;
    bus.flags = index == 0 ? BusInfo::kDefaultActive : 0;
    char name[32];
    if (variant_ == Variant::Effect)
      std::snprintf(name, sizeof name, "%s", index == 0 ? "Main" : "Sidechain");
    else if (index == 0)
      std::snprintf(name, sizeof name, "Main");
    else
      std::snprintf(name, sizeof name, "Insert %d", static_cast<int>(index));
    setString(bus.name, name);
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
  tresult PLUGIN_API setActive(TBool state) override {
    if (!inst_) return kResultFalse;
    inSetActive_ = true;
    if (state)
      inst_->activate(sampleRate_, maxBlock_);
    else
      inst_->deactivate();
    inSetActive_ = false;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    if (!s || !inst_) return kInvalidArgument;
    std::string data;
    char buf[65536];
    for (;;) {
      int32 got = 0;
      if (s->read(buf, static_cast<int32>(sizeof buf), &got) != kResultOk || got <= 0) break;
      data.append(buf, static_cast<size_t>(got));
    }
    return inst_->loadState(data) ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    if (!s || !inst_) return kInvalidArgument;
    const std::string data = inst_->saveState();
    size_t done = 0;
    while (done < data.size()) {
      int32 wrote = 0;
      const int32 chunk = static_cast<int32>(std::min<size_t>(data.size() - done, 1 << 20));
      if (s->write(const_cast<char*>(data.data() + done), chunk, &wrote) != kResultOk || wrote <= 0) return kResultFalse;
      done += static_cast<size_t>(wrote);
    }
    return kResultOk;
  }

  // ---- IAudioProcessor ----
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 numIns, SpeakerArrangement* out, int32 numOuts) override {
    for (int32 i = 0; i < numIns; ++i)
      if (in[i] != SpeakerArr::kStereo) return kResultFalse;
    for (int32 i = 0; i < numOuts; ++i)
      if (out[i] != SpeakerArr::kStereo) return kResultFalse;
    return numIns == getBusCount(kAudio, kInput) && numOuts == getBusCount(kAudio, kOutput) ? kResultTrue : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
    if (index < 0 || index >= getBusCount(kAudio, dir)) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return inst_ ? static_cast<uint32>(std::max(0, inst_->latency())) : 0; }
  tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
    sampleRate_ = setup.sampleRate > 0 ? setup.sampleRate : 44100.0;
    maxBlock_ = std::max<int32>(1, setup.maxSamplesPerBlock);
    if (inst_) inst_->setOffline(setup.processMode == kOffline);
    return kResultOk;
  }
  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
  uint32 PLUGIN_API getTailSamples() override { return kInfiniteTail; }
  tresult PLUGIN_API process(ProcessData& d) override {
    if (!inst_) return kResultFalse;
    if (d.numSamples <= 0 || d.numOutputs < 1 || !d.outputs[0].channelBuffers32 || d.outputs[0].numChannels < 1) {
      readParamsOnly(d);
      return kResultOk;
    }
    gl::plugin::ProcessArgs a;
    a.frames = d.numSamples;
    a.outL = d.outputs[0].channelBuffers32[0];
    a.outR = d.outputs[0].numChannels > 1 ? d.outputs[0].channelBuffers32[1] : a.outL;
    float* xl[kExtraOutputs];
    float* xr[kExtraOutputs];
    if (variant_ == Variant::Instrument) {
      int extra = 0;
      for (int32 b = 1; b < d.numOutputs && extra < kExtraOutputs; ++b) {
        const AudioBusBuffers& ob = d.outputs[b];
        if (ob.numChannels < 1 || !ob.channelBuffers32) break;
        xl[extra] = ob.channelBuffers32[0];
        xr[extra] = ob.numChannels > 1 ? ob.channelBuffers32[1] : ob.channelBuffers32[0];
        ++extra;
      }
      a.extraCount = extra;
      a.extraL = xl;
      a.extraR = xr;
    } else {
      if (d.numInputs > 0 && d.inputs[0].numChannels > 0 && d.inputs[0].channelBuffers32) {
        a.inL = d.inputs[0].channelBuffers32[0];
        a.inR = d.inputs[0].numChannels > 1 ? d.inputs[0].channelBuffers32[1] : a.inL;
      }
      if (d.numInputs > 1 && d.inputs[1].numChannels > 0 && d.inputs[1].channelBuffers32) {
        a.scL = d.inputs[1].channelBuffers32[0];
        a.scR = d.inputs[1].numChannels > 1 ? d.inputs[1].channelBuffers32[1] : a.scL;
      }
    }
    // Parameter points first (macros, MIDI controllers): at the same offset
    // a controller (pitch bend set before a note) applies before the note.
    int n = addParamEvents(d, 0);
    if (IEventList* el = d.inputEvents) {
      const int32 count = el->getEventCount();
      for (int32 i = 0; i < count && n < kMaxEvents; ++i) {
        Steinberg::Vst::Event ve{};
        if (el->getEvent(i, ve) != kResultOk) continue;
        GlEvent e;
        e.kind = GlEvent::Midi;
        e.offset = std::clamp<int32>(ve.sampleOffset, 0, d.numSamples - 1);
        if (ve.type == Steinberg::Vst::Event::kNoteOnEvent) {
          const int vel = static_cast<int>(std::lround(std::clamp(ve.noteOn.velocity, 0.f, 1.f) * 127.f));
          e.midi[0] = static_cast<uint8_t>(vel > 0 ? 0x90 | (ve.noteOn.channel & 15) : 0x80 | (ve.noteOn.channel & 15));
          e.midi[1] = static_cast<uint8_t>(std::clamp<int16>(ve.noteOn.pitch, 0, 127));
          e.midi[2] = static_cast<uint8_t>(std::max(vel, 0));
        } else if (ve.type == Steinberg::Vst::Event::kNoteOffEvent) {
          e.midi[0] = static_cast<uint8_t>(0x80 | (ve.noteOff.channel & 15));
          e.midi[1] = static_cast<uint8_t>(std::clamp<int16>(ve.noteOff.pitch, 0, 127));
          e.midi[2] = static_cast<uint8_t>(std::lround(std::clamp(ve.noteOff.velocity, 0.f, 1.f) * 127.f));
        } else if (ve.type == Steinberg::Vst::Event::kPolyPressureEvent) {
          e.midi[0] = static_cast<uint8_t>(0xA0 | (ve.polyPressure.channel & 15));
          e.midi[1] = static_cast<uint8_t>(std::clamp<int16>(ve.polyPressure.pitch, 0, 127));
          e.midi[2] = static_cast<uint8_t>(std::lround(std::clamp(ve.polyPressure.pressure, 0.f, 1.f) * 127.f));
        } else {
          continue;
        }
        events_[n++] = e;
      }
    }
    // Notes, controllers and macro points in time order; insertion sort (stable, no allocation on the audio thread).
    for (int i = 1; i < n; ++i) {
      const GlEvent e = events_[i];
      int j = i;
      while (j > 0 && events_[j - 1].offset > e.offset) {
        events_[j] = events_[j - 1];
        --j;
      }
      events_[j] = e;
    }
    a.events = events_;
    a.eventCount = n;
    if (const ProcessContext* c = d.processContext) {
      a.transport.valid = true;
      a.transport.playing = (c->state & ProcessContext::kPlaying) != 0;
      a.transport.hasTempo = (c->state & ProcessContext::kTempoValid) != 0;
      a.transport.bpm = c->tempo;
      a.transport.hasPosition = (c->state & ProcessContext::kProjectTimeMusicValid) != 0;
      a.transport.beats = c->projectTimeMusic;
    }
    inst_->process(a);
    return kResultOk;
  }

  // ---- IEditController ----
  tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }  // one object: setState did it
  int32 PLUGIN_API getParameterCount() override { return inst_ ? inst_->paramCount() + midiParams() : 0; }
  tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
    if (!inst_ || index < 0 || index >= getParameterCount()) return kInvalidArgument;
    info = ParameterInfo{};
    const int own = inst_->paramCount();
    if (index >= own) {  // hidden MIDI controller parameter (IMidiMapping)
      const int k = index - own, ch = k / kMidiCtrls, ctrl = k % kMidiCtrls;
      char name[48];
      if (ctrl < 128)
        std::snprintf(name, sizeof name, "MIDI CC %d (ch %d)", ctrl, ch + 1);
      else
        std::snprintf(name, sizeof name, "%s (ch %d)", ctrl == kAfterTouch ? "Channel pressure" : "Pitch bend", ch + 1);
      info.id = kMidiBase + static_cast<ParamID>(k);
      setString(info.title, name);
      setString(info.shortTitle, name);
      info.stepCount = ctrl == kPitchBend ? 16383 : 127;
      info.defaultNormalizedValue = ctrl == kPitchBend ? 0.5 : 0.0;
      info.unitId = 0;
      info.flags = ParameterInfo::kIsHidden;
      return kResultOk;
    }
    info.id = inst_->paramId(index);
    info.unitId = 0;  // the root unit
    info.flags = ParameterInfo::kCanAutomate;
    if (!inst_->single()) {
      setString(info.title, inst_->macroName(index));
      setString(info.shortTitle, "M" + std::to_string(index + 1));
      info.stepCount = 0;
      info.defaultNormalizedValue = 0;
      return kResultOk;
    }
    const gl::plugin::HostParam h = inst_->paramInfo(index);
    setString(info.title, h.name);
    setString(info.shortTitle, h.name);
    setString(info.units, h.unit);
    info.stepCount = inst_->paramSteps(index);
    info.defaultNormalizedValue = h.def;
    if (h.bypass) info.flags |= ParameterInfo::kIsBypass;
    if (h.choice) info.flags |= ParameterInfo::kIsList;
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 s) override {
    if (isMidiParam(id)) {
      uint8_t m[3];
      midiFromParam(id, v, m);
      const int x = (m[0] & 0xF0) == 0xE0 ? ((m[2] << 7) | m[1]) - 8192 : (m[0] & 0xF0) == 0xD0 ? m[1] : m[2];
      setString(s, std::to_string(x));
      return kResultOk;
    }
    const int k = inst_ ? inst_->paramIndex(id) : -1;
    if (k < 0) return kInvalidArgument;
    setString(s, inst_->paramText(k, v));
    return kResultOk;
  }
  tresult PLUGIN_API getParamValueByString(ParamID id, TChar* text, ParamValue& v) override {
    const int own = inst_ ? inst_->paramIndex(id) : -1;
    if ((own < 0 && !isMidiParam(id)) || !text) return kInvalidArgument;
    const std::string t = narrow(text);
    if (own >= 0) {
      double n = 0;
      if (!inst_->paramFromText(own, t, n)) return kResultFalse;
      v = n;
      return kResultOk;
    }
    char* end = nullptr;
    double x = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return kResultFalse;
    if (isMidiParam(id)) {
      const int ctrl = static_cast<int>(id - kMidiBase) % kMidiCtrls;
      v = ctrl == kPitchBend ? std::clamp((x + 8192.0) / 16383.0, 0.0, 1.0) : std::clamp(x / 127.0, 0.0, 1.0);
      return kResultOk;
    }
    return kResultFalse;
  }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override {
    const int k = inst_ && inst_->single() ? inst_->paramIndex(id) : -1;
    return k >= 0 ? inst_->paramToPlain(k, v) : v;
  }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) override {
    const int k = inst_ && inst_->single() ? inst_->paramIndex(id) : -1;
    return k >= 0 ? inst_->paramFromPlain(k, v) : v;
  }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
    if (isMidiParam(id)) return midiValues_[id - kMidiBase].load(std::memory_order_relaxed);
    const int k = inst_ ? inst_->paramIndex(id) : -1;
    if (k < 0) return 0;
    idleSoon();
    return inst_->paramValue(k);
  }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
    if (isMidiParam(id)) {  // the controller's last value (the MIDI arrives through process)
      midiValues_[id - kMidiBase].store(static_cast<float>(std::clamp(v, 0.0, 1.0)), std::memory_order_relaxed);
      return kResultOk;
    }
    const int k = inst_ ? inst_->paramIndex(id) : -1;
    if (k < 0) return kInvalidArgument;
    inst_->setParamValue(k, v);
    return kResultOk;
  }

  // ---- IMidiMapping ----
  tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel, CtrlNumber ctrl, ParamID& id) override {
    if (!takesNotes() || busIndex != 0 || channel < 0 || channel > 15 || ctrl < 0 || ctrl >= kMidiCtrls) return kResultFalse;
    id = kMidiBase + static_cast<ParamID>(channel * kMidiCtrls + ctrl);
    return kResultTrue;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
    handler_ = h;
    return kResultOk;
  }
  IPlugView* PLUGIN_API createView(FIDString name) override {
    if (!inst_ || !name || std::strcmp(name, ViewType::kEditor) != 0 || !gl::plugin::viewSupported(gl::plugin::nativeWindowApi())) return nullptr;
    return new GridlineView(*inst_);
  }

 private:
  // Instruments, MIDI effects and the Gridline workstation take notes.
  bool takesNotes() const { return type_.empty() || variant_ == Variant::Instrument || midiInput_; }
  int midiParams() const { return takesNotes() ? kMidiParams : 0; }
  void idle() {
    if (inst_) inst_->idle();
    lastIdle_ = seconds();
  }
  // Main-thread calls keep the housekeeping going where no timer runs (Linux without an editor).
  void idleSoon() {
    if (seconds() - lastIdle_ > 0.1) idle();
  }
  void readParamsOnly(ProcessData& d) {
    const int n = addParamEvents(d, 0);
    for (int i = 0; i < n; ++i)
      if (events_[i].kind == GlEvent::Macro)
        inst_->setParamValue(events_[i].macro, events_[i].value);
      else  // no audio this call: the controller takes effect with the next block
        inst_->queueMidi(events_[i].midi);
  }
  // Macro points and MIDI controller points (as MIDI messages) of a block.
  int addParamEvents(ProcessData& d, int n) {
    IParameterChanges* pc = d.inputParameterChanges;
    if (!pc) return n;
    const int32 count = pc->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
      IParamValueQueue* q = pc->getParameterData(i);
      if (!q) continue;
      const ParamID id = q->getParameterId();
      const int index = inst_->paramIndex(id);
      const bool macro = index >= 0;
      if (!macro && (!isMidiParam(id) || !takesNotes())) continue;
      const int32 points = q->getPointCount();
      for (int32 k = 0; k < points && n < kMaxEvents; ++k) {
        int32 off = 0;
        ParamValue v = 0;
        if (q->getPoint(k, off, v) != kResultOk) continue;
        GlEvent e;
        e.offset = std::max<int32>(0, std::min<int32>(off, std::max<int32>(0, d.numSamples - 1)));
        if (macro) {
          e.kind = GlEvent::Macro;
          e.macro = index;
          e.value = static_cast<float>(v);
        } else {
          e.kind = GlEvent::Midi;
          midiFromParam(id, v, e.midi);
          midiValues_[id - kMidiBase].store(static_cast<float>(v), std::memory_order_relaxed);
        }
        events_[n++] = e;
      }
    }
    return n;
  }

  Variant variant_;
  std::string type_;
  bool sidechain_ = true, midiInput_ = false;
  std::unique_ptr<Instance> inst_;
  IComponentHandler* handler_ = nullptr;
  double sampleRate_ = 44100.0;
  int32 maxBlock_ = 1024;
  double lastIdle_ = 0;
  bool inSetActive_ = false;
  GlEvent events_[kMaxEvents];
  std::atomic<float> midiValues_[kMidiParams];  // last value of each MIDI controller parameter
#if defined(__APPLE__)
  CFRunLoopTimerRef timer_ = nullptr;
#endif
};

// ---------------------------------------------------------------- factory

class Factory final : public RefCounted, public IPluginFactory2 {
 public:
  GRIDLINE_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    GRIDLINE_QI(IPluginFactory2)
    GRIDLINE_QI(IPluginFactory)
    if (sameIid(iid, FUnknown::iid)) {
      *obj = static_cast<FUnknown*>(static_cast<IPluginFactory2*>(this));
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
    *info = PFactoryInfo{};
    std::snprintf(info->vendor, sizeof info->vendor, "Gridline");
    std::snprintf(info->url, sizeof info->url, "https://github.com/lolitemaultes/Gridline");
    info->flags = PFactoryInfo::kUnicode;
    return kResultOk;
  }
#ifndef GRIDLINE_SUITE
  int32 PLUGIN_API countClasses() override { return 2; }
#else
  int32 PLUGIN_API countClasses() override { return static_cast<int32>(gl::suite::members(GRIDLINE_SUITE).size()); }
#endif
  tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
    PClassInfo2 i2{};
    if (getClassInfo2(index, &i2) != kResultOk) return kInvalidArgument;
    *info = PClassInfo{};
    std::memcpy(info->cid, i2.cid, sizeof(TUID));
    info->cardinality = i2.cardinality;
    std::snprintf(info->category, sizeof info->category, "%s", i2.category);
    std::snprintf(info->name, sizeof info->name, "%s", i2.name);
    return kResultOk;
  }
#ifdef GRIDLINE_SUITE
  tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
    const auto& ms = gl::suite::members(GRIDLINE_SUITE);
    if (index < 0 || index >= static_cast<int32>(ms.size())) return kInvalidArgument;
    const gl::suite::Member& m = ms[static_cast<size_t>(index)];
    *info = PClassInfo2{};
    std::memcpy(info->cid, FUID(m.vst3Uid[0], m.vst3Uid[1], m.vst3Uid[2], m.vst3Uid[3]).toTUID(), sizeof(TUID));
    info->cardinality = PClassInfo::kManyInstances;
    std::snprintf(info->category, sizeof info->category, "%s", kVstAudioEffectClass);
    std::snprintf(info->name, sizeof info->name, "%s", m.name.c_str());
    std::snprintf(info->subCategories, sizeof info->subCategories, "%s", m.vst3Category.c_str());
    std::snprintf(info->vendor, sizeof info->vendor, "Gridline");
    std::snprintf(info->version, sizeof info->version, "%s", GRIDLINE_VERSION_STRING);
    std::snprintf(info->sdkVersion, sizeof info->sdkVersion, "VST 3.8.0");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
    *obj = nullptr;
    FUnknown* inst = nullptr;
    for (const gl::suite::Member& m : gl::suite::members(GRIDLINE_SUITE))
      if (same(cid, FUID(m.vst3Uid[0], m.vst3Uid[1], m.vst3Uid[2], m.vst3Uid[3]))) {
        inst = static_cast<IComponent*>(new Component(m.instrument ? Variant::Instrument : Variant::Effect, m.type, m.sidechain, m.midiInput));
        break;
      }
    if (!inst) return kNoInterface;
    const tresult r = inst->queryInterface(iid, obj);
    inst->release();
    return r;
  }
#else
  tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
    if (index < 0 || index > 1) return kInvalidArgument;
    *info = PClassInfo2{};
    std::memcpy(info->cid, (index == 0 ? kInstrumentUid : kEffectUid).toTUID(), sizeof(TUID));
    info->cardinality = PClassInfo::kManyInstances;
    std::snprintf(info->category, sizeof info->category, "%s", kVstAudioEffectClass);
    std::snprintf(info->name, sizeof info->name, "%s", index == 0 ? "Gridline" : "Gridline FX");
    std::snprintf(info->subCategories, sizeof info->subCategories, "%s", index == 0 ? "Instrument|Synth|Sampler" : "Fx|Mixing");
    std::snprintf(info->vendor, sizeof info->vendor, "Gridline");
    std::snprintf(info->version, sizeof info->version, "%s", GRIDLINE_VERSION_STRING);
    std::snprintf(info->sdkVersion, sizeof info->sdkVersion, "VST 3.8.0");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
    *obj = nullptr;
    FUnknown* inst = nullptr;
    if (same(cid, kInstrumentUid))
      inst = static_cast<IComponent*>(new Component(Variant::Instrument, "", true, false));
    else if (same(cid, kEffectUid))
      inst = static_cast<IComponent*>(new Component(Variant::Effect, "", true, false));
    if (!inst) return kNoInterface;
    const tresult r = inst->queryInterface(iid, obj);
    inst->release();
    return r;
  }
#endif
};

}  // namespace

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return new Factory; }
#if defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit() { return true; }
#elif defined(_WIN32)
SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif
}

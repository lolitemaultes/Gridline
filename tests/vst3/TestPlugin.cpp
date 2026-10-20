// Gridline — minimal VST3 plugin used by the host tests (built in-tree as
// GridlineTest.vst3; never installed). Written against the SDK interface
// headers only. Two audio classes:
//   "Gridline Test Synth"  instrument, separate controller class, connection
//                          points + messages, note expression (tuning); a
//                          single sine voice started/stopped sample-accurately;
//                          MIDI mapping (pitch bend, CC 7), a program list
//                          (IUnitInfo) and a second output bus ("Sub", one
//                          octave lower); its own interface on macOS (a
//                          fixed 360 x 220 NSView).
//   "Gridline Test Gain"   effect, single component (processor = controller),
//                          sidechain bus, 32 samples reported latency.
#include <atomic>
#include <cmath>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <cstdio>
#include <cstring>
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "Probe.h"
#include "pluginterfaces/gui/iplugview.h"
#if defined(__APPLE__)
#include "../common/TestView.h"
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IMessage)
DEF_CLASS_IID(IAttributeList)
DEF_CLASS_IID(IHostApplication)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(INoteExpressionController)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IEventList)
DEF_CLASS_IID(IMidiMapping)
DEF_CLASS_IID(IUnitInfo)
}  // namespace Vst
}  // namespace Steinberg

namespace {

GridlineVst3Probe gProbe;
void noteContext(const ProcessData& d) {
  gProbe.hadContext = d.processContext != nullptr;
  if (d.processContext) gProbe.context = *d.processContext;
  gProbe.processMode = d.processMode;
  ++gProbe.blocks;
}

const FUID kSynthUid(0x47524944, 0x4C494E45, 0x54535931, 0x00000001);
const FUID kSynthCtrlUid(0x47524944, 0x4C494E45, 0x54535931, 0x00000002);
const FUID kGainUid(0x47524944, 0x4C494E45, 0x54535931, 0x00000003);

enum SynthParam : ParamID { kGain = 0, kWave = 1, kPing = 2, kMode = 3, kPoke = 5, kBend = 6, kProgram = 7 };
// Programs: gain per program.
const double kProgramGain[3] = {0.25, 0.5, 1.0};
const char* const kProgramNames[3] = {"Quiet", "Normal", "Loud"};
enum GainParam : ParamID { kFxGain = 10, kFxSide = 11, kFxBypass = 12 };

bool same(const TUID a, const FUID& b) { return std::memcmp(a, b.toTUID(), sizeof(TUID)) == 0; }

template <class T>
bool readValue(IBStream* s, T& v) {
  int32 got = 0;
  return s && s->read(&v, sizeof v, &got) == kResultOk && got == static_cast<int32>(sizeof v);
}
template <class T>
void writeValue(IBStream* s, T v) {
  s->write(&v, sizeof v, nullptr);
}
void setString(String128 dst, const char* src) {
  size_t i = 0;
  for (; src[i] && i < 127; ++i) dst[i] = static_cast<TChar>(src[i]);
  dst[i] = 0;
}
void setText(String128 dst, const char* fmt, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, fmt, v);
  setString(dst, buf);
}

// Reference counting shared by every class below.
class RefCounted {
 public:
  virtual ~RefCounted() = default;

 protected:
  uint32 add() { return ++refs_; }
  uint32 rel() {
    uint32 r = --refs_;
    if (r == 0) delete this;
    return r;
  }

 private:
  std::atomic<uint32> refs_{1};
};

#define TEST_REFCOUNT                                      \
  uint32 PLUGIN_API addRef() override { return add(); }   \
  uint32 PLUGIN_API release() override { return rel(); }

#define TEST_QI(I)                       \
  if (same(iid, I::iid)) {               \
    *obj = static_cast<I*>(this);        \
    addRef();                            \
    return kResultOk;                    \
  }

// --------------------------------------------------------------- interface

#if defined(__APPLE__)
// The synth's own interface (macOS): a fixed 360 x 220 NSView.
class TestView final : public RefCounted, public IPlugView {
 public:
  ~TestView() override { removed(); }
  TEST_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    TEST_QI(IPlugView)
    if (same(iid, FUnknown::iid)) {
      *obj = static_cast<IPlugView*>(this);
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
    return type && std::strcmp(type, kPlatformTypeNSView) == 0 ? kResultTrue : kResultFalse;
  }
  tresult PLUGIN_API attached(void* parent, FIDString type) override {
    if (isPlatformTypeSupported(type) != kResultTrue || view_) return kResultFalse;
    view_ = gridlineTestViewAttach(parent, kW, kH, "VST3 test interface", 0.55f, 0.24f, 0.62f);
    if (!view_) return kResultFalse;
    ++gProbe.views;
    return kResultOk;
  }
  tresult PLUGIN_API removed() override {
    if (view_) {
      gridlineTestViewRemove(view_);
      view_ = nullptr;
      --gProbe.views;
    }
    return kResultOk;
  }
  tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
  tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
  tresult PLUGIN_API getSize(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    *r = ViewRect(0, 0, kW, kH);
    return kResultOk;
  }
  tresult PLUGIN_API onSize(ViewRect*) override { return kResultOk; }
  tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
  tresult PLUGIN_API setFrame(IPlugFrame*) override { return kResultOk; }
  tresult PLUGIN_API canResize() override { return kResultFalse; }
  tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
    if (!r) return kInvalidArgument;
    *r = ViewRect(0, 0, kW, kH);
    return kResultOk;
  }

 private:
  static constexpr int kW = 360, kH = 220;
  void* view_ = nullptr;
};
#endif

// --------------------------------------------------------------- synth processor

class SynthProcessor final : public RefCounted, public IComponent, public IAudioProcessor, public IConnectionPoint {
 public:
  TEST_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    TEST_QI(IComponent)
    TEST_QI(IAudioProcessor)
    TEST_QI(IConnectionPoint)
    if (same(iid, IPluginBase::iid)) {
      *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
      addRef();
      return kResultOk;
    }
    if (same(iid, FUnknown::iid)) {
      *obj = static_cast<FUnknown*>(static_cast<IComponent*>(this));
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  // IPluginBase
  tresult PLUGIN_API initialize(FUnknown* context) override {
    host_ = context;
    return kResultOk;
  }
  tresult PLUGIN_API terminate() override {
    host_ = nullptr;
    return kResultOk;
  }
  // IComponent
  tresult PLUGIN_API getControllerClassId(TUID classId) override {
    std::memcpy(classId, kSynthCtrlUid.toTUID(), sizeof(TUID));
    return kResultOk;
  }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type == kAudio && dir == kOutput) return 2;
    if (type == kEvent && dir == kInput) return 1;
    return 0;
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus.mediaType = type;
    bus.direction = dir;
    bus.channelCount = type == kAudio ? 2 : 16;
    setString(bus.name, type == kAudio ? (index == 0 ? "Out" : "Sub") : "Notes");
    bus.busType = index == 0 ? kMain : kAux;
    bus.flags = index == 0 ? BusInfo::kDefaultActive : 0;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
  tresult PLUGIN_API setActive(TBool state) override {
    active_ = state;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    double g, w, m;
    int32 n;
    if (!readValue(s, g) || !readValue(s, w) || !readValue(s, n) || !readValue(s, m)) return kResultFalse;
    gain_ = g;
    wave_ = w;
    notesPlayed_ = n;
    mode_ = m;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    if (!s) return kInvalidArgument;
    writeValue(s, gain_);
    writeValue(s, wave_);
    writeValue(s, notesPlayed_);
    writeValue(s, mode_);
    return kResultOk;
  }
  // IAudioProcessor
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) override {
    return numIns == 0 && numOuts >= 1 && outputs[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
    if (dir != kOutput || index < 0 || index > 1) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return 0; }
  tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
    sr_ = setup.sampleRate;
    return kResultOk;
  }
  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
  uint32 PLUGIN_API getTailSamples() override { return 0; }
  tresult PLUGIN_API process(ProcessData& d) override {
    noteContext(d);
    if (IParameterChanges* pc = d.inputParameterChanges)
      for (int32 i = 0; i < pc->getParameterCount(); ++i) {
        IParamValueQueue* q = pc->getParameterData(i);
        int32 off;
        ParamValue v;
        if (!q || q->getPointCount() <= 0 || q->getPoint(q->getPointCount() - 1, off, v) != kResultOk) continue;
        if (q->getParameterId() == kGain) gain_ = v;
        if (q->getParameterId() == kWave) wave_ = v;
        if (q->getParameterId() == kMode) mode_ = v;
        if (q->getParameterId() == kBend) midiBend_ = (v - 0.5) * 4.0;  // +-2 semitones
        if (q->getParameterId() == kProgram) gain_ = kProgramGain[v >= 0.75 ? 2 : v >= 0.25 ? 1 : 0];
      }
    if (d.numOutputs < 1 || d.outputs[0].numChannels < 2) return kResultOk;
    float* L = d.outputs[0].channelBuffers32[0];
    float* R = d.outputs[0].channelBuffers32[1];
    float* SL = d.numOutputs > 1 && d.outputs[1].numChannels >= 2 ? d.outputs[1].channelBuffers32[0] : nullptr;
    float* SR = SL ? d.outputs[1].channelBuffers32[1] : nullptr;
    IEventList* ev = d.inputEvents;
    int32 evCount = ev ? ev->getEventCount() : 0, ei = 0;
    Event e{};
    for (int32 i = 0; i < d.numSamples; ++i) {
      while (ei < evCount && ev->getEvent(ei, e) == kResultOk && e.sampleOffset <= i) {
        ++ei;
        if (e.type == Event::kNoteOnEvent) {
          noteId_ = e.noteOn.noteId;
          pitch_ = e.noteOn.pitch + e.noteOn.tuning / 100.0;
          bend_ = 0;
          amp_ = e.noteOn.velocity;
          phase_ = 0;
          ++notesPlayed_;
        } else if (e.type == Event::kNoteOffEvent && (e.noteOff.noteId == noteId_ || e.noteOff.noteId == -1)) {
          amp_ = 0;
        } else if (e.type == Event::kNoteExpressionValueEvent && e.noteExpressionValue.typeId == kTuningTypeID &&
                   e.noteExpressionValue.noteId == noteId_) {
          bend_ = 240.0 * (e.noteExpressionValue.value - 0.5);
        }
      }
      float s = 0, sub = 0;
      if (amp_ > 0) {
        double f = 440.0 * std::pow(2.0, (pitch_ + bend_ + midiBend_ - 69.0) / 12.0);
        phase_ += f / sr_;
        phase_ -= std::floor(phase_);
        subPhase_ += 0.5 * f / sr_;
        subPhase_ -= std::floor(subPhase_);
        double x = std::sin(2.0 * M_PI * phase_);
        if (wave_ >= 0.5) x = x >= 0 ? 1.0 : -1.0;
        s = static_cast<float>(x * amp_ * gain_);
        sub = static_cast<float>(std::sin(2.0 * M_PI * subPhase_) * amp_ * gain_);
      }
      L[i] = R[i] = s;
      if (SL) SL[i] = SR[i] = sub;
    }
    return kResultOk;
  }
  // IConnectionPoint: greets the controller with a message on connect.
  tresult PLUGIN_API connect(IConnectionPoint* other) override {
    peer_ = other;
    IHostApplication* app = nullptr;
    if (host_ && host_->queryInterface(IHostApplication::iid.toTUID(), reinterpret_cast<void**>(&app)) == kResultOk && app) {
      void* obj = nullptr;
      TUID cid, iid;
      std::memcpy(cid, IMessage::iid.toTUID(), sizeof cid);
      std::memcpy(iid, IMessage::iid.toTUID(), sizeof iid);
      if (app->createInstance(cid, iid, &obj) == kResultOk && obj) {
        auto* m = static_cast<IMessage*>(obj);
        m->setMessageID("Ping");
        m->getAttributes()->setInt("value", 42);
        other->notify(m);
        m->release();
      }
      app->release();
    }
    return kResultOk;
  }
  tresult PLUGIN_API disconnect(IConnectionPoint*) override {
    peer_ = nullptr;
    return kResultOk;
  }
  tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

 private:
  FUnknown* host_ = nullptr;
  IConnectionPoint* peer_ = nullptr;
  bool active_ = false;
  double sr_ = 44100, gain_ = 0.5, wave_ = 0, mode_ = 0;
  int32 notesPlayed_ = 0;
  int32 noteId_ = -1;
  double pitch_ = 60, bend_ = 0, midiBend_ = 0, amp_ = 0, phase_ = 0, subPhase_ = 0;
};

// --------------------------------------------------------------- synth controller

class SynthController final : public RefCounted,
                              public IEditController,
                              public IConnectionPoint,
                              public INoteExpressionController,
                              public IMidiMapping,
                              public IUnitInfo {
 public:
  TEST_REFCOUNT
  ~SynthController() override {
    if (handler_) handler_->release();
  }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    TEST_QI(IEditController)
    TEST_QI(IConnectionPoint)
    TEST_QI(INoteExpressionController)
    TEST_QI(IMidiMapping)
    TEST_QI(IUnitInfo)
    if (same(iid, IPluginBase::iid) || same(iid, FUnknown::iid)) {
      *obj = static_cast<IPluginBase*>(static_cast<IEditController*>(this));
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
  tresult PLUGIN_API terminate() override { return kResultOk; }
  tresult PLUGIN_API setComponentState(IBStream* s) override {
    double g, w, m;
    int32 n;
    if (!readValue(s, g) || !readValue(s, w) || !readValue(s, n) || !readValue(s, m)) return kResultFalse;
    v_[kGain] = g;
    v_[kWave] = w;
    v_[kMode] = m;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* s) override {
    int32 z;
    if (!readValue(s, z)) return kResultFalse;
    zoom_ = z;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    writeValue(s, zoom_);
    return kResultOk;
  }
  int32 PLUGIN_API getParameterCount() override { return 7; }
  tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
    static const ParamID ids[] = {kGain, kWave, kPing, kMode, kPoke, kBend, kProgram};
    if (index < 0 || index >= 7) return kInvalidArgument;
    info = ParameterInfo{};
    info.id = ids[index];
    info.flags = ParameterInfo::kCanAutomate;
    switch (info.id) {
      case kGain:
        setString(info.title, "Gain");
        setString(info.units, "x");
        info.defaultNormalizedValue = 0.5;
        break;
      case kWave:
        setString(info.title, "Square");
        info.stepCount = 1;
        break;
      case kPing:
        setString(info.title, "Ping");
        info.flags = ParameterInfo::kIsReadOnly;
        break;
      case kMode:
        setString(info.title, "Mode");
        info.stepCount = 2;
        info.flags |= ParameterInfo::kIsList;
        break;
      case kPoke:
        setString(info.title, "Poke");
        info.flags = ParameterInfo::kIsHidden;
        break;
      case kBend:  // driven by MIDI pitch bend (IMidiMapping)
        setString(info.title, "Bend");
        info.flags = ParameterInfo::kIsHidden;
        info.defaultNormalizedValue = 0.5;
        break;
      case kProgram:
        setString(info.title, "Program");
        info.stepCount = 2;
        info.flags = ParameterInfo::kIsProgramChange | ParameterInfo::kIsList | ParameterInfo::kIsHidden;
        info.unitId = kRootUnitId;
        break;
      default:
        break;
    }
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 s) override {
    if (id == kWave) {
      setString(s, v >= 0.5 ? "Square" : "Sine");
    } else if (id == kMode) {
      static const char* names[] = {"Soft", "Hard", "Fold"};
      setString(s, names[static_cast<int>(std::lround(v * 2))]);
    } else {
      setText(s, "%.2f", v * (id == kGain ? 1.0 : 100.0));
    }
    return kResultOk;
  }
  tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id < 8 ? v_[id] : 0.0; }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
    if (id >= 8) return kInvalidArgument;
    v_[id] = v;
    if (id == kProgram) v_[kGain] = kProgramGain[v >= 0.75 ? 2 : v >= 0.25 ? 1 : 0];
    if (id == kPoke && handler_) {  // test hook: behave like an edit in the plugin's own editor
      handler_->beginEdit(kGain);
      handler_->performEdit(kGain, v);
      v_[kGain] = v;
      handler_->endEdit(kGain);
    }
    return kResultOk;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
    if (h) h->addRef();
    if (handler_) handler_->release();
    handler_ = h;
    return kResultOk;
  }
#if defined(__APPLE__)
  IPlugView* PLUGIN_API createView(FIDString name) override {
    return name && std::strcmp(name, ViewType::kEditor) == 0 ? new TestView : nullptr;
  }
#else
  IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }
#endif
  // IConnectionPoint
  tresult PLUGIN_API connect(IConnectionPoint*) override { return kResultOk; }
  tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultOk; }
  tresult PLUGIN_API notify(IMessage* m) override {
    int64 v = 0;
    if (m && std::strcmp(m->getMessageID(), "Ping") == 0 && m->getAttributes()->getInt("value", v) == kResultOk)
      v_[kPing] = static_cast<double>(v) / 100.0;
    return kResultOk;
  }
  // INoteExpressionController
  int32 PLUGIN_API getNoteExpressionCount(int32 bus, int16 channel) override { return bus == 0 && channel == 0 ? 1 : 0; }
  tresult PLUGIN_API getNoteExpressionInfo(int32 bus, int16 channel, int32 index, NoteExpressionTypeInfo& info) override {
    if (bus != 0 || channel != 0 || index != 0) return kInvalidArgument;
    info = NoteExpressionTypeInfo{};
    info.typeId = kTuningTypeID;
    setString(info.title, "Tuning");
    info.valueDesc.defaultValue = 0.5;
    info.valueDesc.minimum = 0;
    info.valueDesc.maximum = 1;
    info.flags = NoteExpressionTypeInfo::kIsBipolar;
    return kResultOk;
  }
  tresult PLUGIN_API getNoteExpressionStringByValue(int32, int16, NoteExpressionTypeID, NoteExpressionValue, String128) override {
    return kNotImplemented;
  }
  tresult PLUGIN_API getNoteExpressionValueByString(int32, int16, NoteExpressionTypeID, const TChar*, NoteExpressionValue&) override {
    return kNotImplemented;
  }
  // IMidiMapping: pitch bend → Bend, CC 7 → Gain.
  tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16 channel, CtrlNumber ctrl, ParamID& id) override {
    if (bus != 0 || channel != 0) return kResultFalse;
    if (ctrl == kPitchBend) id = kBend;
    else if (ctrl == kCtrlVolume) id = kGain;
    else return kResultFalse;
    return kResultTrue;
  }
  // IUnitInfo: the root unit with one program list.
  int32 PLUGIN_API getUnitCount() override { return 1; }
  tresult PLUGIN_API getUnitInfo(int32 index, UnitInfo& info) override {
    if (index != 0) return kInvalidArgument;
    info = UnitInfo{};
    info.id = kRootUnitId;
    info.parentUnitId = kNoParentUnitId;
    setString(info.name, "Root");
    info.programListId = 1;
    return kResultOk;
  }
  int32 PLUGIN_API getProgramListCount() override { return 1; }
  tresult PLUGIN_API getProgramListInfo(int32 index, ProgramListInfo& info) override {
    if (index != 0) return kInvalidArgument;
    info.id = 1;
    setString(info.name, "Factory");
    info.programCount = 3;
    return kResultOk;
  }
  tresult PLUGIN_API getProgramName(ProgramListID list, int32 index, String128 name) override {
    if (list != 1 || index < 0 || index > 2) return kInvalidArgument;
    setString(name, kProgramNames[index]);
    return kResultOk;
  }
  tresult PLUGIN_API getProgramInfo(ProgramListID, int32, Steinberg::Vst::CString, String128) override { return kNotImplemented; }
  tresult PLUGIN_API hasProgramPitchNames(ProgramListID, int32) override { return kResultFalse; }
  tresult PLUGIN_API getProgramPitchName(ProgramListID, int32, int16, String128) override { return kNotImplemented; }
  UnitID PLUGIN_API getSelectedUnit() override { return kRootUnitId; }
  tresult PLUGIN_API selectUnit(UnitID) override { return kResultOk; }
  tresult PLUGIN_API getUnitByBus(MediaType, BusDirection, int32, int32, UnitID& unit) override {
    unit = kRootUnitId;
    return kResultOk;
  }
  tresult PLUGIN_API setUnitProgramData(int32, int32, IBStream*) override { return kNotImplemented; }

 private:
  double v_[8] = {0.5, 0, 0, 0, 0, 0, 0.5, 0.5};
  int32 zoom_ = 100;
  IComponentHandler* handler_ = nullptr;
};

// --------------------------------------------------------------- gain (single component)

class GainEffect final : public RefCounted, public IComponent, public IAudioProcessor, public IEditController {
 public:
  TEST_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    TEST_QI(IComponent)
    TEST_QI(IAudioProcessor)
    TEST_QI(IEditController)
    if (same(iid, IPluginBase::iid) || same(iid, FUnknown::iid)) {
      *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
  tresult PLUGIN_API terminate() override { return kResultOk; }
  tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type != kAudio) return 0;
    return dir == kInput ? 2 : 1;
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (type != kAudio || index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus.mediaType = type;
    bus.direction = dir;
    bus.channelCount = 2;
    setString(bus.name, index == 0 ? "Main" : "Sidechain");
    bus.busType = index == 0 ? kMain : kAux;
    bus.flags = index == 0 ? BusInfo::kDefaultActive : 0;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType, BusDirection dir, int32 index, TBool state) override {
    if (dir == kInput && index == 1) scActive_ = state;
    return kResultOk;
  }
  tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
  tresult PLUGIN_API setState(IBStream* s) override {
    double g, sc;
    if (!readValue(s, g) || !readValue(s, sc)) return kResultFalse;
    gain_ = g;
    side_ = sc;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* s) override {
    writeValue(s, gain_);
    writeValue(s, side_);
    return kResultOk;
  }
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 numIns, SpeakerArrangement* out, int32 numOuts) override {
    return numIns == 2 && numOuts == 1 && in[0] == SpeakerArr::kStereo && in[1] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo
               ? kResultTrue
               : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement& arr) override {
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return 32; }
  tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
  uint32 PLUGIN_API getTailSamples() override { return 0; }
  tresult PLUGIN_API process(ProcessData& d) override {
    noteContext(d);
    // Sample-accurate gain: every point of the gain queue applies from its offset.
    IParamValueQueue* gq = nullptr;
    if (IParameterChanges* pc = d.inputParameterChanges)
      for (int32 i = 0; i < pc->getParameterCount(); ++i) {
        IParamValueQueue* q = pc->getParameterData(i);
        if (!q || q->getPointCount() <= 0) continue;
        int32 off;
        ParamValue v;
        if (q->getParameterId() == kFxGain) gq = q;
        if (q->getParameterId() == kFxSide && q->getPoint(q->getPointCount() - 1, off, v) == kResultOk) side_ = v;
        if (q->getParameterId() == kFxBypass && q->getPoint(q->getPointCount() - 1, off, v) == kResultOk) bypass_ = v >= 0.5;
      }
    if (d.numInputs < 1 || d.numOutputs < 1) return kResultOk;
    int32 gi = 0, gn = gq ? gq->getPointCount() : 0;
    for (int32 i = 0; i < d.numSamples; ++i) {
      while (gi < gn) {
        int32 off;
        ParamValue v;
        gq->getPoint(gi, off, v);
        if (off > i) break;
        gain_ = v;
        ++gi;
      }
      for (int c = 0; c < 2; ++c) {
        float x = d.inputs[0].channelBuffers32[c][i];
        float sc = d.numInputs > 1 && d.inputs[1].numChannels > c ? d.inputs[1].channelBuffers32[c][i] : 0.0f;
        d.outputs[0].channelBuffers32[c][i] = bypass_ ? x : static_cast<float>(x * gain_ * 2.0 + sc * side_);
      }
    }
    return kResultOk;
  }
  // IEditController
  tresult PLUGIN_API setComponentState(IBStream* s) override {
    double g, sc;
    if (!readValue(s, g) || !readValue(s, sc)) return kResultFalse;
    gain_ = g;
    side_ = sc;
    return kResultOk;
  }
  // setState/getState are shared with IComponent (same signature): one state.
  int32 PLUGIN_API getParameterCount() override { return 3; }
  tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
    if (index < 0 || index > 2) return kInvalidArgument;
    info = ParameterInfo{};
    info.id = static_cast<ParamID>(kFxGain + index);
    info.flags = index == 2 ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass) : ParameterInfo::kCanAutomate;
    info.stepCount = index == 2 ? 1 : 0;
    info.defaultNormalizedValue = index == 0 ? 0.5 : 0.0;
    setString(info.title, index == 0 ? "Level" : index == 1 ? "Sidechain" : "Bypass");
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue v, String128 s) override {
    setText(s, "%.3f", v);
    return kResultOk;
  }
  tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
    return id == kFxGain ? gain_ : id == kFxSide ? side_ : id == kFxBypass ? (bypass_ ? 1.0 : 0.0) : 0.0;
  }
  tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }  // processor owns the values
  tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
  IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

 private:
  double gain_ = 0.5, side_ = 0;
  bool bypass_ = false, scActive_ = false;
};

// --------------------------------------------------------------- factory

class Factory final : public RefCounted, public IPluginFactory2 {
 public:
  TEST_REFCOUNT
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    TEST_QI(IPluginFactory2)
    TEST_QI(IPluginFactory)
    if (same(iid, FUnknown::iid)) {
      *obj = static_cast<FUnknown*>(this);
      addRef();
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
    *info = PFactoryInfo{};
    std::strcpy(info->vendor, "Gridline Tests");
    info->flags = PFactoryInfo::kUnicode;
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 3; }
  tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
    PClassInfo2 i2{};
    if (getClassInfo2(index, &i2) != kResultOk) return kInvalidArgument;
    std::memcpy(info->cid, i2.cid, sizeof(TUID));
    info->cardinality = i2.cardinality;
    std::strcpy(info->category, i2.category);
    std::strcpy(info->name, i2.name);
    return kResultOk;
  }
  tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
    if (index < 0 || index > 2) return kInvalidArgument;
    *info = PClassInfo2{};
    const FUID& id = index == 0 ? kSynthUid : index == 1 ? kSynthCtrlUid : kGainUid;
    std::memcpy(info->cid, id.toTUID(), sizeof(TUID));
    info->cardinality = PClassInfo::kManyInstances;
    std::strcpy(info->category, index == 1 ? kVstComponentControllerClass : kVstAudioEffectClass);
    std::strcpy(info->name, index == 2 ? "Gridline Test Gain" : "Gridline Test Synth");
    std::strcpy(info->subCategories, index == 0 ? "Instrument|Synth" : index == 2 ? "Fx|Dynamics" : "");
    std::strcpy(info->vendor, "Gridline Tests");
    std::strcpy(info->version, "1.0.0");
    std::strcpy(info->sdkVersion, "VST 3.8.1");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
    *obj = nullptr;
    FUnknown* inst = nullptr;
    if (same(cid, kSynthUid)) inst = static_cast<IComponent*>(new SynthProcessor);
    else if (same(cid, kSynthCtrlUid)) inst = static_cast<IEditController*>(new SynthController);
    else if (same(cid, kGainUid)) inst = static_cast<IComponent*>(new GainEffect);
    if (!inst) return kNoInterface;
    tresult r = inst->queryInterface(iid, obj);
    inst->release();
    return r;
  }
};

}  // namespace

extern "C" {
SMTG_EXPORT_SYMBOL GridlineVst3Probe* gridline_vst3_probe() { return &gProbe; }
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return new Factory; }
#if defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif
}

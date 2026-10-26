// Gridline — Audio Unit (v2) entry point of Gridline as a plugin (macOS).
// Written against Apple's AudioToolbox / AudioUnit system headers only: an
// AudioComponentPlugInInterface whose Lookup hands out one function per
// selector (no SDK base classes). Two components in Gridline.component
// (Info.plist "AudioComponents", factory GridlineAUFactory):
//   aumu GrIn GrLn  "Gridline"     instrument: MIDI in, stereo out + 15
//                                  stereo outputs (mixer inserts 1-15)
//   aufx GrFx GrLn  "Gridline FX"  effect: stereo in (mixer insert 1) +
//                                  sidechain input, stereo out
// Parameters are the 16 macros; ClassInfo holds the whole project (the
// plugin state in "data"); transport comes from the host callbacks; MIDI and
// scheduled parameter changes apply at their sample offsets; latency is the
// project's; the Cocoa view is Gridline's interface.
//
// J-131: built with GRIDLINE_SUITE="<suite id>", the component is a Gridline
// Suite bundle instead: one AudioComponent per member of the suite
// (core/PluginSuite.h; Info.plist from `gridline suite-info --plist`), each one
// built-in instrument (aumu) or effect (aufx; aumf when it takes MIDI) with
// its own parameters, factory presets, state and view. Effects with a
// sidechain have a second input element.
#if defined(__APPLE__)
#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AudioUnitUtilities.h>
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "PluginCore.h"
#include "PluginView.h"
#ifdef GRIDLINE_SUITE
#include "../core/PluginInfo.h"
#include "../core/PluginSuite.h"
#endif

// Objective-C class names are global in a process: each bundle has its own.
#ifndef GRIDLINE_AU_FACTORY_CLASS
#define GRIDLINE_AU_FACTORY_CLASS GridlineAuViewFactory
#endif
#ifndef GRIDLINE_AU_VIEW_CLASS
#define GRIDLINE_AU_VIEW_CLASS GridlineAuView
#endif
#ifndef GRIDLINE_AU_FACTORY
#define GRIDLINE_AU_FACTORY GridlineAUFactory
#endif
#define GRIDLINE_AU_STR2(x) #x
#define GRIDLINE_AU_STR(x) GRIDLINE_AU_STR2(x)

using gl::plugin::Instance;
using gl::plugin::kExtraOutputs;
using gl::plugin::kMacros;
using gl::plugin::Variant;

namespace {

constexpr OSType fourcc(const char (&s)[5]) {
  return (static_cast<OSType>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<OSType>(static_cast<unsigned char>(s[1])) << 16) |
         (static_cast<OSType>(static_cast<unsigned char>(s[2])) << 8) | static_cast<OSType>(static_cast<unsigned char>(s[3]));
}
constexpr OSType kManufacturer = fourcc("GrLn");
constexpr OSType kSubInstrument = fourcc("GrIn");
constexpr OSType kSubEffect = fourcc("GrFx");
// The view factory asks the unit for its instance (private property).
constexpr AudioUnitPropertyID kGridlineInstanceProperty = 64731;
constexpr int kMaxEvents = 2048;
constexpr int kMaxListeners = 64;

AudioStreamBasicDescription canonical(double sr, UInt32 ch) {
  AudioStreamBasicDescription f{};
  f.mSampleRate = sr;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
  f.mBytesPerPacket = f.mBytesPerFrame = 4;
  f.mFramesPerPacket = 1;
  f.mChannelsPerFrame = ch;
  f.mBitsPerChannel = 32;
  return f;
}

class AuPlugin;

// Dev checks: GRIDLINE_PLUGIN_WINDOW_LOG=<file> gets a line when the Cocoa view opens and closes.
void devLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void devLog(const char* fmt, ...) {
  const char* path = std::getenv("GRIDLINE_PLUGIN_WINDOW_LOG");
  if (!path || !*path) return;
  if (FILE* f = std::fopen(path, "a")) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
  }
}

// Events from MIDIEvent / SetParameter / ScheduleParameters (one producer
// thread at a time, the render thread consumes).
class EventRing {
 public:
  bool push(const gl::plugin::Event& e) {
    const unsigned h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= kMaxEvents) return false;
    buf_[h % kMaxEvents] = e;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  int drain(gl::plugin::Event* out, int max) {
    unsigned t = tail_.load(std::memory_order_relaxed);
    const unsigned h = head_.load(std::memory_order_acquire);
    int n = 0;
    for (; t != h && n < max; ++t) out[n++] = buf_[t % kMaxEvents];
    tail_.store(t, std::memory_order_release);
    return n;
  }
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  gl::plugin::Event buf_[kMaxEvents];
  std::atomic<unsigned> head_{0}, tail_{0};
};

}  // namespace

@interface GRIDLINE_AU_FACTORY_CLASS : NSObject <AUCocoaUIBase>
@end
@interface GRIDLINE_AU_VIEW_CLASS : NSView {
 @public
  AuPlugin* owner;
  gl::plugin::View* view;
}
- (void)detachGridline;
@end

namespace {

// The interface is the component's `self` pointer: the first member of the
// first (standard-layout) base, so it converts back to the plugin.
struct AuInterface {
  AudioComponentPlugInInterface iface{};
};

class AuPlugin : public AuInterface {
 public:
  // `type`: a Gridline Suite member ("" = the Gridline workstation), with its codes.
  AuPlugin(Variant v, std::string type, OSType auType, OSType auSubtype, bool sidechain, bool midiInput)
      : variant_(v), type_(std::move(type)), auType_(auType), auSubtype_(auSubtype), sidechain_(sidechain) {
    iface.Open = &AuPlugin::open;
    iface.Close = &AuPlugin::close;
    // MIDI selectors for the instrument and for effects that take MIDI (an 'aumf').
    iface.Lookup = v == Variant::Effect && !midiInput ? &AuPlugin::lookupEffect : &AuPlugin::lookupInstrument;
    iface.reserved = nullptr;
    outFormats_.assign(static_cast<size_t>(outputCount()), canonical(sampleRate_, 2));
    inFormats_.assign(static_cast<size_t>(inputCount()), canonical(sampleRate_, 2));
    inputs_.resize(static_cast<size_t>(inputCount()));
  }
  ~AuPlugin() {
    detachViews();
    if (timer_) {
      CFRunLoopTimerInvalidate(timer_);
      CFRelease(timer_);
    }
    if (contextName_) CFRelease(contextName_);
    if (presetName_) CFRelease(presetName_);
    for (AUPreset& pr : factory_)
      if (pr.presetName) CFRelease(pr.presetName);
    inst_.reset();
  }

  Instance* instance() { return inst_.get(); }
  void addView(GRIDLINE_AU_VIEW_CLASS* v) { views_.push_back(v); }
  void removeView(GRIDLINE_AU_VIEW_CLASS* v) { views_.erase(std::remove(views_.begin(), views_.end(), v), views_.end()); }

 private:
  struct Listener {
    AudioUnitPropertyID id;
    AudioUnitPropertyListenerProc proc;
    void* data;
  };
  struct Notify {
    AURenderCallback proc;
    void* data;
  };
  struct Input {
    AURenderCallbackStruct callback{nullptr, nullptr};
    AudioUnitConnection connection{nullptr, 0, 0};
  };

  int inputCount() const { return variant_ == Variant::Effect ? (sidechain_ ? 2 : 1) : 0; }  // main + sidechain
  int outputCount() const { return variant_ == Variant::Effect || !type_.empty() ? 1 : 1 + kExtraOutputs; }
  int paramCount() const { return inst_ ? inst_->paramCount() : 0; }
  // AU values: stepped parameters (toggles, choices) count their steps; the
  // others are normalized 0..1 (their text is the real value). No allocation.
  double toNormalized(int k, double v) const {
    const int steps = inst_->paramSteps(k);
    if (steps > 0) return inst_->paramFromPlain(k, std::clamp(std::round(v), 0.0, static_cast<double>(steps)));
    return std::clamp(v, 0.0, 1.0);
  }
  float fromNormalized(int k, double n) const {
    const int steps = inst_->paramSteps(k);
    if (steps > 0) return static_cast<float>(std::clamp(std::round(inst_->paramToPlain(k, n)), 0.0, static_cast<double>(steps)));
    return static_cast<float>(std::clamp(n, 0.0, 1.0));
  }
  void refreshParams() {
    for (int k = 0; k < paramCount(); ++k) params_[static_cast<size_t>(k)].store(fromNormalized(k, inst_->paramValue(k)));
  }
  static AuPlugin* self(void* p) { return static_cast<AuPlugin*>(reinterpret_cast<AuInterface*>(p)); }

  // ---------------------------------------------------------------- component
  static OSStatus open(void* p, AudioComponentInstance unit) {
    AuPlugin* s = self(p);
    s->unit_ = unit;
    s->inst_ = s->type_.empty() ? std::make_unique<Instance>(s->variant_) : std::make_unique<Instance>(s->variant_, s->type_);
    if (!s->type_.empty() && !s->inst_->single()) return kAudioUnitErr_FailedInitialization;  // not a plugin of this build
    s->params_.reset(new std::atomic<float>[static_cast<size_t>(s->paramCount())]);
    s->refreshParams();
    s->inst_->onLatencyChanged = [s]() { s->notify(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0); };
    if (s->inst_->single()) {  // J-131: the plugin's own parameters; edits come inside their gestures
      s->inst_->onMacrosChanged = [s]() {
        s->refreshParams();
        AudioUnitParameter any{s->unit_, kAUParameterListener_AnyParameter, kAudioUnitScope_Global, 0};
        AUParameterListenerNotify(nullptr, nullptr, &any);
      };
      s->inst_->onMacroEdited = [s](int k, double v) {
        s->params_[static_cast<size_t>(k)].store(s->fromNormalized(k, v));
        AudioUnitParameter prm{s->unit_, s->inst_->paramId(k), kAudioUnitScope_Global, 0};
        AUParameterListenerNotify(nullptr, nullptr, &prm);
      };
      s->inst_->onParamGesture = [s](int k, bool begin) {
        AudioUnitEvent ev{};
        ev.mArgument.mParameter = AudioUnitParameter{s->unit_, s->inst_->paramId(k), kAudioUnitScope_Global, 0};
        ev.mEventType = begin ? kAudioUnitEvent_BeginParameterChangeGesture : kAudioUnitEvent_EndParameterChangeGesture;
        AUEventListenerNotify(nullptr, nullptr, &ev);
      };
    } else {
      s->inst_->onMacrosChanged = [s]() { s->notify(kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0); };
      s->inst_->onMacroEdited = [s](int k, double v) {
        s->params_[static_cast<size_t>(k)].store(static_cast<float>(v));
        AudioUnitParameter prm{s->unit_, static_cast<AudioUnitParameterID>(k), kAudioUnitScope_Global, 0};
        AudioUnitEvent ev{};
        ev.mArgument.mParameter = prm;
        ev.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
        AUEventListenerNotify(nullptr, nullptr, &ev);
        AUParameterListenerNotify(nullptr, nullptr, &prm);
        ev.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
        AUEventListenerNotify(nullptr, nullptr, &ev);
      };
    }
    // Main-thread housekeeping while no view is open.
    CFRunLoopTimerContext ctx{};
    ctx.info = s;
    s->timer_ = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.05, 0.03, 0, 0,
                                     [](CFRunLoopTimerRef, void* info) {
                                       AuPlugin* a = static_cast<AuPlugin*>(info);
                                       if (a->inst_) a->inst_->idle();
                                     },
                                     &ctx);
    if (s->timer_) CFRunLoopAddTimer(CFRunLoopGetMain(), s->timer_, kCFRunLoopCommonModes);
    return noErr;
  }
  static OSStatus close(void* p) {
    delete self(p);
    return noErr;
  }

  static AudioComponentMethod lookupInstrument(SInt16 selector) {
    switch (selector) {
      case kMusicDeviceMIDIEventSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::midiEvent);
      case kMusicDeviceSysExSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::sysEx);
      default: return lookupEffect(selector);
    }
  }
  static AudioComponentMethod lookupEffect(SInt16 selector) {
    switch (selector) {
      case kAudioUnitInitializeSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::initialize);
      case kAudioUnitUninitializeSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::uninitialize);
      case kAudioUnitGetPropertyInfoSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::getPropertyInfo);
      case kAudioUnitGetPropertySelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::getProperty);
      case kAudioUnitSetPropertySelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::setProperty);
      case kAudioUnitAddPropertyListenerSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::addListener);
      case kAudioUnitRemovePropertyListenerSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::removeListener);
      case kAudioUnitRemovePropertyListenerWithUserDataSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::removeListenerData);
      case kAudioUnitAddRenderNotifySelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::addRenderNotify);
      case kAudioUnitRemoveRenderNotifySelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::removeRenderNotify);
      case kAudioUnitGetParameterSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::getParameter);
      case kAudioUnitSetParameterSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::setParameter);
      case kAudioUnitScheduleParametersSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::scheduleParameters);
      case kAudioUnitRenderSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::render);
      case kAudioUnitResetSelect: return reinterpret_cast<AudioComponentMethod>(&AuPlugin::reset);
      default: return nullptr;
    }
  }

  // ---------------------------------------------------------------- lifecycle
  static OSStatus initialize(void* p) {
    AuPlugin* s = self(p);
    if (s->initialized_) return noErr;
    // Every bus runs at the output's rate.
    s->sampleRate_ = s->outFormats_[0].mSampleRate;
    for (const auto& f : s->inFormats_)
      if (f.mSampleRate != s->sampleRate_) return kAudioUnitErr_FormatNotSupported;
    for (const auto& f : s->outFormats_)
      if (f.mSampleRate != s->sampleRate_) return kAudioUnitErr_FormatNotSupported;
    const size_t n = s->maxFrames_;
    s->inBuf_.assign(static_cast<size_t>(s->inputCount()) * 2 * n, 0.f);
    s->outBuf_.assign(static_cast<size_t>(s->outputCount()) * 2 * n, 0.f);
    s->events_.clear();
    s->inst_->activate(s->sampleRate_, static_cast<int>(n));
    s->initialized_ = true;
    s->delivered_ = 0;
    s->haveCycle_ = false;
    return noErr;
  }
  static OSStatus uninitialize(void* p) {
    AuPlugin* s = self(p);
    if (s->initialized_) s->inst_->deactivate();
    s->initialized_ = false;
    return noErr;
  }
  static OSStatus reset(void* p, AudioUnitScope, AudioUnitElement) {
    AuPlugin* s = self(p);
    s->haveCycle_ = false;
    s->delivered_ = 0;
    return noErr;
  }

  // ---------------------------------------------------------------- properties
  bool validElement(AudioUnitScope scope, AudioUnitElement el) const {
    if (scope == kAudioUnitScope_Global) return el == 0;
    if (scope == kAudioUnitScope_Input) return el < static_cast<UInt32>(inputCount());
    if (scope == kAudioUnitScope_Output) return el < static_cast<UInt32>(outputCount());
    return false;
  }

  static OSStatus getPropertyInfo(void* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, UInt32* size, Boolean* writable) {
    AuPlugin* s = self(p);
    UInt32 sz = 0;
    Boolean w = false;
    const bool global = scope == kAudioUnitScope_Global;
    switch (id) {
      case kAudioUnitProperty_ClassInfo:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(CFPropertyListRef);
        w = true;
        break;
      case kAudioUnitProperty_StreamFormat:
        if (!s->validElement(scope, el) || global) return global ? kAudioUnitErr_InvalidScope : kAudioUnitErr_InvalidElement;
        sz = sizeof(AudioStreamBasicDescription);
        w = true;
        break;
      case kAudioUnitProperty_SampleRate:
        if (!global && !s->validElement(scope, el)) return kAudioUnitErr_InvalidElement;
        sz = sizeof(Float64);
        w = true;
        break;
      case kAudioUnitProperty_ElementCount:
        sz = sizeof(UInt32);
        break;
      case kAudioUnitProperty_SupportedNumChannels:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AUChannelInfo);
        break;
      case kAudioUnitProperty_MaximumFramesPerSlice:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(UInt32);
        w = true;
        break;
      case kAudioUnitProperty_Latency:
      case kAudioUnitProperty_TailTime:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(Float64);
        break;
      case kAudioUnitProperty_ParameterList:
        sz = global ? static_cast<UInt32>(sizeof(AudioUnitParameterID) * static_cast<size_t>(s->paramCount())) : 0;
        break;
      case kAudioUnitProperty_ParameterInfo:
        if (!global) return kAudioUnitErr_InvalidScope;
        if (s->inst_->paramIndex(el) < 0) return kAudioUnitErr_InvalidElement;
        sz = sizeof(AudioUnitParameterInfo);
        break;
      case kAudioUnitProperty_ParameterValueStrings: {  // choices' names
        if (!global) return kAudioUnitErr_InvalidScope;
        const int k = s->inst_->paramIndex(el);
        if (k < 0 || !s->inst_->single() || s->inst_->paramInfo(k).choices.empty()) return kAudioUnitErr_InvalidProperty;
        sz = sizeof(CFArrayRef);
        break;
      }
      case kAudioUnitProperty_FactoryPresets:
        if (!global) return kAudioUnitErr_InvalidScope;
        if (!s->inst_->single()) return kAudioUnitErr_InvalidProperty;
        sz = sizeof(CFArrayRef);
        break;
      case kAudioUnitProperty_ParameterStringFromValue:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AudioUnitParameterStringFromValue);
        break;
      case kAudioUnitProperty_ParameterValueFromString:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AudioUnitParameterValueFromString);
        break;
      case kAudioUnitProperty_HostCallbacks:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(HostCallbackInfo);
        w = true;
        break;
      case kAudioUnitProperty_SetRenderCallback:
      case kAudioUnitProperty_MakeConnection:
        if (scope != kAudioUnitScope_Input) return kAudioUnitErr_InvalidScope;
        if (!s->validElement(scope, el)) return kAudioUnitErr_InvalidElement;
        sz = id == kAudioUnitProperty_SetRenderCallback ? sizeof(AURenderCallbackStruct) : sizeof(AudioUnitConnection);
        w = true;
        break;
      case kAudioUnitProperty_CocoaUI:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AudioUnitCocoaViewInfo);
        break;
      case kAudioUnitProperty_ShouldAllocateBuffer:
        if (global || !s->validElement(scope, el)) return global ? kAudioUnitErr_InvalidScope : kAudioUnitErr_InvalidElement;
        sz = sizeof(UInt32);
        w = true;
        break;
      case kAudioUnitProperty_InPlaceProcessing:
      case kAudioUnitProperty_OfflineRender:
      case kAudioUnitProperty_RenderQuality:
      case kAudioUnitProperty_LastRenderError:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(UInt32);
        w = id != kAudioUnitProperty_LastRenderError;
        break;
      case kAudioUnitProperty_BypassEffect:
        if (s->variant_ != Variant::Effect) return kAudioUnitErr_InvalidProperty;
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(UInt32);
        w = true;
        break;
      case kAudioUnitProperty_PresentPreset:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AUPreset);
        w = true;
        break;
      case kAudioUnitProperty_ContextName:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(CFStringRef);
        w = true;
        break;
      case kAudioUnitProperty_PresentationLatency:
        if (global || !s->validElement(scope, el)) return global ? kAudioUnitErr_InvalidScope : kAudioUnitErr_InvalidElement;
        sz = sizeof(Float64);
        w = true;
        break;
      case kGridlineInstanceProperty:
        if (!global) return kAudioUnitErr_InvalidScope;
        sz = sizeof(AuPlugin*);
        break;
      default:
        return kAudioUnitErr_InvalidProperty;
    }
    if (size) *size = sz;
    if (writable) *writable = w;
    return noErr;
  }

  template <class T>
  static OSStatus put(void* out, UInt32* size, const T& v) {
    if (!out || !size || *size < sizeof(T)) return kAudioUnitErr_InvalidPropertyValue;
    std::memcpy(out, &v, sizeof(T));
    *size = sizeof(T);
    return noErr;
  }

  static OSStatus getProperty(void* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void* out, UInt32* size) {
    AuPlugin* s = self(p);
    UInt32 need = 0;
    Boolean w = false;
    if (OSStatus st = getPropertyInfo(p, id, scope, el, &need, &w); st != noErr) return st;
    switch (id) {
      case kAudioUnitProperty_ClassInfo: return put(out, size, static_cast<CFPropertyListRef>(s->classInfo()));
      case kAudioUnitProperty_StreamFormat:
        return put(out, size, scope == kAudioUnitScope_Input ? s->inFormats_[el] : s->outFormats_[el]);
      case kAudioUnitProperty_SampleRate: {
        const AudioStreamBasicDescription& f = scope == kAudioUnitScope_Input ? s->inFormats_[el] : s->outFormats_[el];
        return put(out, size, scope == kAudioUnitScope_Global ? s->outFormats_[0].mSampleRate : f.mSampleRate);
      }
      case kAudioUnitProperty_ElementCount: {
        UInt32 n = scope == kAudioUnitScope_Input ? static_cast<UInt32>(s->inputCount())
                   : scope == kAudioUnitScope_Output ? static_cast<UInt32>(s->outputCount())
                   : scope == kAudioUnitScope_Global ? 1u
                                                     : 0u;
        return put(out, size, n);
      }
      case kAudioUnitProperty_SupportedNumChannels: {
        AUChannelInfo ci{static_cast<SInt16>(s->variant_ == Variant::Effect ? 2 : 0), 2};
        return put(out, size, ci);
      }
      case kAudioUnitProperty_MaximumFramesPerSlice: return put(out, size, s->maxFrames_);
      case kAudioUnitProperty_Latency:
        return put(out, size, static_cast<Float64>(s->inst_->latency()) / std::max(1.0, s->sampleRate_));
      // Suite effects ring on (reverbs, delays): hosts keep rendering after the input stops.
      case kAudioUnitProperty_TailTime: return put(out, size, static_cast<Float64>(!s->type_.empty() && s->variant_ == Variant::Effect ? 10.0 : 0.0));
      case kAudioUnitProperty_ParameterList: {
        if (scope != kAudioUnitScope_Global) {
          if (size) *size = 0;
          return noErr;
        }
        if (!out || !size || *size < need) return kAudioUnitErr_InvalidPropertyValue;
        auto* ids = static_cast<AudioUnitParameterID*>(out);
        for (int k = 0; k < s->paramCount(); ++k) ids[k] = s->inst_->paramId(k);
        *size = need;
        return noErr;
      }
      case kAudioUnitProperty_ParameterInfo: {
        AudioUnitParameterInfo pi{};
        const int k = s->inst_->paramIndex(el);
        const gl::plugin::HostParam h = s->inst_->paramInfo(k);
        std::snprintf(pi.name, sizeof pi.name, "%s", h.name.c_str());
        pi.cfNameString = CFStringCreateWithCString(nullptr, h.name.c_str(), kCFStringEncodingUTF8);
        pi.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_HasCFNameString |
                   kAudioUnitParameterFlag_CFNameRelease;
        pi.unit = kAudioUnitParameterUnit_Generic;
        pi.minValue = 0;
        pi.maxValue = 1;
        pi.defaultValue = 0;
        if (s->inst_->single()) {
          pi.flags |= kAudioUnitParameterFlag_ValuesHaveStrings;
          const int steps = s->inst_->paramSteps(k);
          if (steps > 0) {
            pi.unit = h.toggle ? kAudioUnitParameterUnit_Boolean : kAudioUnitParameterUnit_Indexed;
            pi.maxValue = static_cast<AudioUnitParameterValue>(steps);
          }
          pi.defaultValue = s->fromNormalized(k, h.def);
        }
        return put(out, size, pi);
      }
      case kAudioUnitProperty_ParameterValueStrings: {
        const gl::plugin::HostParam h = s->inst_->paramInfo(s->inst_->paramIndex(el));
        CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, static_cast<CFIndex>(h.choices.size()), &kCFTypeArrayCallBacks);
        for (const std::string& c : h.choices) {
          CFStringRef str = CFStringCreateWithCString(nullptr, c.c_str(), kCFStringEncodingUTF8);
          CFArrayAppendValue(arr, str);
          CFRelease(str);
        }
        return put(out, size, static_cast<CFArrayRef>(arr));  // the caller releases it
      }
      case kAudioUnitProperty_FactoryPresets: {
        s->makeFactoryPresets();
        CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, static_cast<CFIndex>(s->factory_.size()), nullptr);
        for (AUPreset& pr : s->factory_) CFArrayAppendValue(arr, &pr);
        return put(out, size, static_cast<CFArrayRef>(arr));  // the caller releases it (the presets stay ours)
      }
      case kAudioUnitProperty_ParameterStringFromValue: {
        if (!out || !size || *size < need) return kAudioUnitErr_InvalidPropertyValue;
        auto* sv = static_cast<AudioUnitParameterStringFromValue*>(out);
        const int k = s->inst_->paramIndex(sv->inParamID);
        if (k < 0) return kAudioUnitErr_InvalidParameter;
        const double v = sv->inValue ? *sv->inValue : s->params_[static_cast<size_t>(k)].load();
        sv->outString = CFStringCreateWithCString(nullptr, s->inst_->paramText(k, s->toNormalized(k, v)).c_str(), kCFStringEncodingUTF8);
        *size = need;
        return noErr;
      }
      case kAudioUnitProperty_ParameterValueFromString: {
        if (!out || !size || *size < need) return kAudioUnitErr_InvalidPropertyValue;
        auto* vs = static_cast<AudioUnitParameterValueFromString*>(out);
        const int k = s->inst_->paramIndex(vs->inParamID);
        if (k < 0 || !vs->inString) return kAudioUnitErr_InvalidParameter;
        char text[256];
        if (!CFStringGetCString(vs->inString, text, sizeof text, kCFStringEncodingUTF8)) return kAudioUnitErr_InvalidPropertyValue;
        double n = 0;
        if (!s->inst_->paramFromText(k, text, n)) return kAudioUnitErr_InvalidPropertyValue;
        vs->outValue = s->fromNormalized(k, n);
        *size = need;
        return noErr;
      }
      case kAudioUnitProperty_HostCallbacks: return put(out, size, s->host_);
      case kAudioUnitProperty_SetRenderCallback:
        if (el >= s->inputs_.size()) return kAudioUnitErr_InvalidElement;
        return put(out, size, s->inputs_[el].callback);
      case kAudioUnitProperty_MakeConnection:
        if (el >= s->inputs_.size()) return kAudioUnitErr_InvalidElement;
        return put(out, size, s->inputs_[el].connection);
      case kAudioUnitProperty_CocoaUI: {
        AudioUnitCocoaViewInfo info{};
        info.mCocoaAUViewBundleLocation = bundleUrl();
        info.mCocoaAUViewClass[0] = CFStringCreateWithCString(nullptr, GRIDLINE_AU_STR(GRIDLINE_AU_FACTORY_CLASS), kCFStringEncodingUTF8);
        if (!info.mCocoaAUViewBundleLocation) {
          CFRelease(info.mCocoaAUViewClass[0]);
          return kAudioUnitErr_InvalidProperty;
        }
        return put(out, size, info);
      }
      case kAudioUnitProperty_InPlaceProcessing: return put(out, size, static_cast<UInt32>(s->inPlace_ ? 1 : 0));
      case kAudioUnitProperty_ShouldAllocateBuffer: return put(out, size, static_cast<UInt32>(1));
      case kAudioUnitProperty_OfflineRender: return put(out, size, static_cast<UInt32>(s->offline_ ? 1 : 0));
      case kAudioUnitProperty_RenderQuality: return put(out, size, static_cast<UInt32>(kRenderQuality_Max));
      case kAudioUnitProperty_LastRenderError: return put(out, size, static_cast<UInt32>(0));
      case kAudioUnitProperty_BypassEffect: return put(out, size, static_cast<UInt32>(s->bypass_.load() ? 1 : 0));
      case kAudioUnitProperty_PresentPreset: {
        AUPreset pr{s->presetNumber_, s->presetName_ ? static_cast<CFStringRef>(CFRetain(s->presetName_)) : CFSTR("Untitled")};
        return put(out, size, pr);
      }
      case kAudioUnitProperty_ContextName: {
        CFStringRef n = s->contextName_ ? static_cast<CFStringRef>(CFRetain(s->contextName_)) : nullptr;
        return put(out, size, n);
      }
      case kAudioUnitProperty_PresentationLatency: return put(out, size, static_cast<Float64>(0));
      case kGridlineInstanceProperty: return put(out, size, s);
      default: return kAudioUnitErr_InvalidProperty;
    }
  }

  static OSStatus setProperty(void* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void* in, UInt32 size) {
    AuPlugin* s = self(p);
    UInt32 need = 0;
    Boolean w = false;
    if (OSStatus st = getPropertyInfo(p, id, scope, el, &need, &w); st != noErr) return st;
    if (!w) return kAudioUnitErr_PropertyNotWritable;
    if (!in && id != kAudioUnitProperty_SetRenderCallback && id != kAudioUnitProperty_MakeConnection) return kAudioUnitErr_InvalidPropertyValue;
    OSStatus st = noErr;
    switch (id) {
      case kAudioUnitProperty_ClassInfo:
        if (size < sizeof(CFPropertyListRef)) return kAudioUnitErr_InvalidPropertyValue;
        st = s->setClassInfo(*static_cast<const CFPropertyListRef*>(in));
        break;
      case kAudioUnitProperty_StreamFormat: {
        if (size < sizeof(AudioStreamBasicDescription)) return kAudioUnitErr_InvalidPropertyValue;
        const auto& f = *static_cast<const AudioStreamBasicDescription*>(in);
        const bool ok = f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) &&
                        (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) && f.mBitsPerChannel == 32 && f.mChannelsPerFrame == 2 &&
                        f.mSampleRate > 0;
        if (!ok) return kAudioUnitErr_FormatNotSupported;
        AudioStreamBasicDescription& cur = scope == kAudioUnitScope_Input ? s->inFormats_[el] : s->outFormats_[el];
        if (s->initialized_ && f.mSampleRate != cur.mSampleRate) return kAudioUnitErr_Initialized;
        const bool rateChanged = cur.mSampleRate != f.mSampleRate;
        cur = canonical(f.mSampleRate, 2);
        s->notify(id, scope, el);
        if (rateChanged) s->notify(kAudioUnitProperty_SampleRate, scope, el);
        return noErr;
      }
      case kAudioUnitProperty_SampleRate: {
        if (size < sizeof(Float64)) return kAudioUnitErr_InvalidPropertyValue;
        const Float64 sr = *static_cast<const Float64*>(in);
        if (sr <= 0) return kAudioUnitErr_InvalidPropertyValue;
        if (s->initialized_ && sr != s->sampleRate_) return kAudioUnitErr_Initialized;
        if (scope == kAudioUnitScope_Global) {
          for (auto& f : s->inFormats_) f.mSampleRate = sr;
          for (auto& f : s->outFormats_) f.mSampleRate = sr;
        } else {
          (scope == kAudioUnitScope_Input ? s->inFormats_[el] : s->outFormats_[el]).mSampleRate = sr;
          s->notify(kAudioUnitProperty_StreamFormat, scope, el);
        }
        break;
      }
      case kAudioUnitProperty_MaximumFramesPerSlice: {
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        const UInt32 n = *static_cast<const UInt32*>(in);
        if (n == 0) return kAudioUnitErr_InvalidPropertyValue;
        if (s->initialized_ && n != s->maxFrames_) return kAudioUnitErr_Initialized;
        s->maxFrames_ = n;
        break;
      }
      case kAudioUnitProperty_HostCallbacks:
        s->host_ = HostCallbackInfo{};
        std::memcpy(&s->host_, in, std::min<size_t>(size, sizeof(HostCallbackInfo)));
        break;
      case kAudioUnitProperty_SetRenderCallback: {
        if (el >= s->inputs_.size()) return kAudioUnitErr_InvalidElement;
        Input& i = s->inputs_[el];
        if (in && size >= sizeof(AURenderCallbackStruct))
          i.callback = *static_cast<const AURenderCallbackStruct*>(in);
        else
          i.callback = AURenderCallbackStruct{nullptr, nullptr};
        i.connection = AudioUnitConnection{nullptr, 0, 0};
        break;
      }
      case kAudioUnitProperty_MakeConnection: {
        if (el >= s->inputs_.size()) return kAudioUnitErr_InvalidElement;
        Input& i = s->inputs_[el];
        if (in && size >= sizeof(AudioUnitConnection))
          i.connection = *static_cast<const AudioUnitConnection*>(in);
        else
          i.connection = AudioUnitConnection{nullptr, 0, 0};
        i.callback = AURenderCallbackStruct{nullptr, nullptr};
        break;
      }
      case kAudioUnitProperty_InPlaceProcessing:
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        s->inPlace_ = *static_cast<const UInt32*>(in) != 0;
        break;
      case kAudioUnitProperty_ShouldAllocateBuffer:
      case kAudioUnitProperty_RenderQuality:
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        break;
      case kAudioUnitProperty_OfflineRender:
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        s->offline_ = *static_cast<const UInt32*>(in) != 0;
        s->inst_->setOffline(s->offline_);
        break;
      case kAudioUnitProperty_BypassEffect:
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        s->bypass_.store(*static_cast<const UInt32*>(in) != 0);
        break;
      case kAudioUnitProperty_PresentPreset: {
        if (size < sizeof(AUPreset)) return kAudioUnitErr_InvalidPropertyValue;
        const AUPreset& pr = *static_cast<const AUPreset*>(in);
        if (pr.presetNumber >= 0) {  // a factory preset (suite plugins)
          s->makeFactoryPresets();
          if (static_cast<size_t>(pr.presetNumber) >= s->factory_.size() || !s->inst_->loadFactoryPreset(pr.presetNumber))
            return kAudioUnitErr_InvalidPropertyValue;
          if (s->presetName_) CFRelease(s->presetName_);
          s->presetName_ = static_cast<CFStringRef>(CFRetain(s->factory_[static_cast<size_t>(pr.presetNumber)].presetName));
          s->presetNumber_ = pr.presetNumber;
          break;
        }
        if (s->presetName_) CFRelease(s->presetName_);
        s->presetName_ = pr.presetName ? static_cast<CFStringRef>(CFRetain(pr.presetName)) : nullptr;
        s->presetNumber_ = -1;
        break;
      }
      case kAudioUnitProperty_ContextName: {
        if (size < sizeof(CFStringRef)) return kAudioUnitErr_InvalidPropertyValue;
        CFStringRef n = *static_cast<const CFStringRef*>(in);
        if (s->contextName_) CFRelease(s->contextName_);
        s->contextName_ = n ? static_cast<CFStringRef>(CFRetain(n)) : nullptr;
        break;
      }
      case kAudioUnitProperty_PresentationLatency:
        break;
      default:
        return kAudioUnitErr_InvalidProperty;
    }
    if (st == noErr) s->notify(id, scope, el);
    return st;
  }

  // ---------------------------------------------------------------- listeners
  void notify(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    for (int k = 0; k < nListeners_; ++k)
      if (listeners_[k].id == id) listeners_[k].proc(listeners_[k].data, unit_, id, scope, el);
  }
  static OSStatus addListener(void* p, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void* data) {
    AuPlugin* s = self(p);
    if (!proc) return kAudio_ParamError;
    if (s->nListeners_ >= kMaxListeners) return kAudio_MemFullError;
    s->listeners_[s->nListeners_++] = Listener{id, proc, data};
    return noErr;
  }
  OSStatus dropListeners(AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, bool anyData, void* data) {
    int n = 0;
    for (int k = 0; k < nListeners_; ++k) {
      const Listener& l = listeners_[k];
      if (l.id == id && l.proc == proc && (anyData || l.data == data)) continue;
      listeners_[n++] = l;
    }
    nListeners_ = n;
    return noErr;
  }
  static OSStatus removeListener(void* p, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc) {
    return self(p)->dropListeners(id, proc, true, nullptr);
  }
  static OSStatus removeListenerData(void* p, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void* data) {
    return self(p)->dropListeners(id, proc, false, data);
  }
  static OSStatus addRenderNotify(void* p, AURenderCallback proc, void* data) {
    AuPlugin* s = self(p);
    if (!proc) return kAudio_ParamError;
    if (s->nNotify_.load() >= kMaxListeners) return kAudio_MemFullError;
    const int n = s->nNotify_.load();
    s->notify_[n] = Notify{proc, data};
    s->nNotify_.store(n + 1);
    return noErr;
  }
  static OSStatus removeRenderNotify(void* p, AURenderCallback proc, void* data) {
    AuPlugin* s = self(p);
    int n = 0;
    const int total = s->nNotify_.load();
    for (int k = 0; k < total; ++k)
      if (!(s->notify_[k].proc == proc && s->notify_[k].data == data)) s->notify_[n++] = s->notify_[k];
    s->nNotify_.store(n);
    return noErr;
  }

  // ---------------------------------------------------------------- parameters, MIDI
  static OSStatus getParameter(void* p, AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement, AudioUnitParameterValue* out) {
    AuPlugin* s = self(p);
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    const int k = s->inst_ ? s->inst_->paramIndex(id) : -1;
    if (k < 0) return kAudioUnitErr_InvalidParameter;
    if (!out) return kAudio_ParamError;
    *out = s->params_[static_cast<size_t>(k)].load();
    return noErr;
  }
  static OSStatus setParameter(void* p, AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement, AudioUnitParameterValue v, UInt32 offset) {
    AuPlugin* s = self(p);
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    const int k = s->inst_ ? s->inst_->paramIndex(id) : -1;
    if (k < 0) return kAudioUnitErr_InvalidParameter;
    s->queueMacro(k, v, offset);
    return noErr;
  }
  static OSStatus scheduleParameters(void* p, const AudioUnitParameterEvent* evs, UInt32 n) {
    AuPlugin* s = self(p);
    for (UInt32 i = 0; evs && i < n; ++i) {
      const AudioUnitParameterEvent& e = evs[i];
      const int k = s->inst_ ? s->inst_->paramIndex(e.parameter) : -1;
      if (e.scope != kAudioUnitScope_Global || k < 0) return kAudioUnitErr_InvalidParameter;
      if (e.eventType == kParameterEvent_Immediate)
        s->queueMacro(k, e.eventValues.immediate.value, e.eventValues.immediate.bufferOffset);
      else  // ramps: their end value at their start (the engine smooths macro changes)
        s->queueMacro(k, e.eventValues.ramp.endValue,
                      static_cast<UInt32>(std::max<SInt32>(0, e.eventValues.ramp.startBufferOffset)));
    }
    return noErr;
  }
  // Host parameter k (index) set to the AU value v.
  void queueMacro(int k, float v, UInt32 offset) {
    const double n = toNormalized(k, v);
    params_[static_cast<size_t>(k)].store(fromNormalized(k, n));
    gl::plugin::Event e;
    e.kind = gl::plugin::Event::Macro;
    e.macro = k;
    e.value = static_cast<float>(n);
    e.offset = static_cast<int>(offset);
    if (!initialized_ || !events_.push(e)) inst_->setParamValue(k, n);
  }
  static OSStatus midiEvent(void* p, UInt32 status, UInt32 d1, UInt32 d2, UInt32 offset) {
    AuPlugin* s = self(p);
    if ((status & 0xF0) < 0x80 || (status & 0xF0) == 0xF0) return noErr;  // channel messages only
    gl::plugin::Event e;
    e.kind = gl::plugin::Event::Midi;
    e.midi[0] = static_cast<uint8_t>(status);
    e.midi[1] = static_cast<uint8_t>(d1 & 0x7F);
    e.midi[2] = static_cast<uint8_t>(d2 & 0x7F);
    e.offset = static_cast<int>(offset);
    s->events_.push(e);
    return noErr;
  }
  static OSStatus sysEx(void*, const UInt8*, UInt32) { return noErr; }

  // ---------------------------------------------------------------- render
  float* inCh(int bus, int ch) { return inBuf_.data() + (static_cast<size_t>(bus) * 2 + static_cast<size_t>(ch)) * maxFrames_; }
  float* outCh(int bus, int ch) { return outBuf_.data() + (static_cast<size_t>(bus) * 2 + static_cast<size_t>(ch)) * maxFrames_; }

  // Pulls input bus `bus` into inBuf_; false when nothing is connected.
  OSStatus pullInput(int bus, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 frames, bool& connected) {
    connected = false;
    if (bus >= static_cast<int>(inputs_.size())) return noErr;  // J-131: suite effects without a sidechain have one input bus
    Input& in = inputs_[static_cast<size_t>(bus)];
    connected = in.callback.inputProc || in.connection.sourceAudioUnit;
    if (!connected) return noErr;
    struct {
      AudioBufferList list;
      AudioBuffer second;
    } abl{};
    abl.list.mNumberBuffers = 2;
    for (int c = 0; c < 2; ++c) {
      abl.list.mBuffers[c].mNumberChannels = 1;
      abl.list.mBuffers[c].mDataByteSize = frames * sizeof(float);
      abl.list.mBuffers[c].mData = inCh(bus, c);
    }
    AudioUnitRenderActionFlags f = flags ? *flags : 0;
    OSStatus st = in.callback.inputProc
                      ? in.callback.inputProc(in.callback.inputProcRefCon, &f, ts, static_cast<UInt32>(bus), frames, &abl.list)
                      : AudioUnitRender(in.connection.sourceAudioUnit, &f, ts, in.connection.sourceOutputNumber, frames, &abl.list);
    if (st != noErr) return st;
    for (int c = 0; c < 2; ++c) {  // the source may hand back its own buffers
      const UInt32 b = std::min<UInt32>(static_cast<UInt32>(c), abl.list.mNumberBuffers - 1);
      const float* src = static_cast<const float*>(abl.list.mBuffers[b].mData);
      if (src && src != inCh(bus, c)) std::memcpy(inCh(bus, c), src, frames * sizeof(float));
    }
    return noErr;
  }

  OSStatus renderCycle(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 frames) {
    gl::plugin::ProcessArgs a;
    a.frames = static_cast<int>(frames);
    if (variant_ == Variant::Effect) {
      bool main = false, side = false;
      if (OSStatus st = pullInput(0, flags, ts, frames, main); st != noErr) return st;
      if (!main) return kAudioUnitErr_NoConnection;
      if (OSStatus st = pullInput(1, flags, ts, frames, side); st != noErr) side = false;
      a.inL = inCh(0, 0);
      a.inR = inCh(0, 1);
      a.scL = side ? inCh(1, 0) : nullptr;
      a.scR = side ? inCh(1, 1) : nullptr;
      if (bypass_.load()) {
        std::memcpy(outCh(0, 0), a.inL, frames * sizeof(float));
        std::memcpy(outCh(0, 1), a.inR, frames * sizeof(float));
        events_.clear();
        return noErr;
      }
    }
    a.outL = outCh(0, 0);
    a.outR = outCh(0, 1);
    float* xl[kExtraOutputs];
    float* xr[kExtraOutputs];
    a.extraCount = outputCount() - 1;
    for (int k = 0; k < a.extraCount; ++k) {
      xl[k] = outCh(1 + k, 0);
      xr[k] = outCh(1 + k, 1);
    }
    a.extraL = xl;
    a.extraR = xr;
    // Events in time order (stable: arrival order at equal offsets).
    int n = events_.drain(evBuf_, kMaxEvents);
    for (int i = 0; i < n; ++i) evBuf_[i].offset = std::clamp(evBuf_[i].offset, 0, static_cast<int>(frames) - 1);
    for (int i = 1; i < n; ++i) {
      const gl::plugin::Event e = evBuf_[i];
      int j = i;
      while (j > 0 && evBuf_[j - 1].offset > e.offset) {
        evBuf_[j] = evBuf_[j - 1];
        --j;
      }
      evBuf_[j] = e;
    }
    a.events = evBuf_;
    a.eventCount = n;
    // Transport from the host callbacks.
    if (host_.beatAndTempoProc) {
      Float64 beat = 0, tempo = 0;
      if (host_.beatAndTempoProc(host_.hostUserData, &beat, &tempo) == noErr) {
        a.transport.valid = true;
        a.transport.hasPosition = true;
        a.transport.beats = beat;
        a.transport.hasTempo = tempo > 0;
        a.transport.bpm = tempo;
      }
    }
    Boolean playing = false, recording = false, changed = false, cycling = false;
    Float64 sample = 0, cs = 0, ce = 0;
    if (host_.transportStateProc2 && host_.transportStateProc2(host_.hostUserData, &playing, &recording, &changed, &sample, &cycling, &cs, &ce) == noErr) {
      a.transport.valid = true;
      a.transport.playing = playing;
    } else if (host_.transportStateProc && host_.transportStateProc(host_.hostUserData, &playing, &changed, &sample, &cycling, &cs, &ce) == noErr) {
      a.transport.valid = true;
      a.transport.playing = playing;
    }
    inst_->process(a);
    return noErr;
  }

  static OSStatus render(void* p, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    AuPlugin* s = self(p);
    if (!s->initialized_) return kAudioUnitErr_Uninitialized;
    if (bus >= static_cast<UInt32>(s->outputCount())) return kAudioUnitErr_InvalidElement;
    if (frames > s->maxFrames_) return kAudioUnitErr_TooManyFramesToProcess;
    if (!ts || !io) return kAudio_ParamError;
    const int nn = s->nNotify_.load();
    for (int k = 0; k < nn; ++k) {
      AudioUnitRenderActionFlags f = (flags ? *flags : 0) | kAudioUnitRenderAction_PreRender;
      s->notify_[k].proc(s->notify_[k].data, &f, ts, bus, frames, io);
    }
    // One engine run per render cycle feeds every output bus (hosts pull
    // them one after the other with the same time stamp).
    const bool sameTime = (ts->mFlags & kAudioTimeStampSampleTimeValid) && s->haveCycle_ && ts->mSampleTime == s->cycleTime_;
    OSStatus st = noErr;
    if (!sameTime || (s->delivered_ & (1u << bus))) {
      st = s->renderCycle(flags, ts, frames);
      s->cycleTime_ = ts->mSampleTime;
      s->haveCycle_ = true;
      s->delivered_ = 0;
    }
    if (st == noErr) {
      for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
        float* src = s->outCh(static_cast<int>(bus), static_cast<int>(std::min<UInt32>(c, 1)));
        AudioBuffer& b = io->mBuffers[c];
        if (!b.mData) {
          b.mData = src;
        } else if (b.mData != src) {
          std::memcpy(b.mData, src, std::min<size_t>(frames, b.mDataByteSize / sizeof(float)) * sizeof(float));
        }
        b.mDataByteSize = frames * sizeof(float);
      }
      s->delivered_ |= 1u << bus;
    }
    for (int k = 0; k < nn; ++k) {
      AudioUnitRenderActionFlags f = (flags ? *flags : 0) | kAudioUnitRenderAction_PostRender;
      if (st != noErr) f |= kAudioUnitRenderAction_PostRenderError;
      s->notify_[k].proc(s->notify_[k].data, &f, ts, bus, frames, io);
    }
    return st;
  }

  // ---------------------------------------------------------------- state
  OSType subtype() const { return auSubtype_ ? auSubtype_ : variant_ == Variant::Effect ? kSubEffect : kSubInstrument; }
  OSType type() const { return auType_ ? auType_ : variant_ == Variant::Effect ? kAudioUnitType_Effect : kAudioUnitType_MusicDevice; }
  // The plugin's factory presets as AU presets (suite plugins; made once).
  void makeFactoryPresets() {
    if (madeFactory_ || !inst_ || !inst_->pluginInfo()) return;
    madeFactory_ = true;
    const auto& ps = inst_->pluginInfo()->presets;
    for (size_t i = 0; i < ps.size(); ++i)
      factory_.push_back(AUPreset{static_cast<SInt32>(i), CFStringCreateWithCString(nullptr, ps[i].name.c_str(), kCFStringEncodingUTF8)});
  }

  CFDictionaryRef classInfo() {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto num = [&](CFStringRef key, SInt32 v) {
      CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
      CFDictionarySetValue(d, key, n);
      CFRelease(n);
    };
    num(CFSTR(kAUPresetVersionKey), 0);
    num(CFSTR(kAUPresetTypeKey), static_cast<SInt32>(type()));
    num(CFSTR(kAUPresetSubtypeKey), static_cast<SInt32>(subtype()));
    num(CFSTR(kAUPresetManufacturerKey), static_cast<SInt32>(kManufacturer));
    CFDictionarySetValue(d, CFSTR(kAUPresetNameKey), presetName_ ? presetName_ : CFSTR("Untitled"));
    const std::string state = inst_->saveState();
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(state.data()), static_cast<CFIndex>(state.size()));
    CFDictionarySetValue(d, CFSTR(kAUPresetDataKey), data);
    CFRelease(data);
    return d;  // the caller releases it
  }
  OSStatus setClassInfo(CFPropertyListRef plist) {
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
    CFDictionaryRef d = static_cast<CFDictionaryRef>(plist);
    auto num = [&](CFStringRef key, SInt32& out) {
      CFTypeRef v = CFDictionaryGetValue(d, key);
      return v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt32Type, &out);
    };
    SInt32 t = 0, st = 0, m = 0;
    if (!num(CFSTR(kAUPresetTypeKey), t) || !num(CFSTR(kAUPresetSubtypeKey), st) || !num(CFSTR(kAUPresetManufacturerKey), m) ||
        static_cast<OSType>(t) != type() || static_cast<OSType>(st) != subtype() || static_cast<OSType>(m) != kManufacturer)
      return kAudioUnitErr_InvalidPropertyValue;
    CFTypeRef data = CFDictionaryGetValue(d, CFSTR(kAUPresetDataKey));
    if (!data || CFGetTypeID(data) != CFDataGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
    const std::string state(reinterpret_cast<const char*>(CFDataGetBytePtr(static_cast<CFDataRef>(data))),
                            static_cast<size_t>(CFDataGetLength(static_cast<CFDataRef>(data))));
    if (!inst_->loadState(state)) return kAudioUnitErr_InvalidPropertyValue;
    CFTypeRef name = CFDictionaryGetValue(d, CFSTR(kAUPresetNameKey));
    if (presetName_) CFRelease(presetName_);
    presetName_ = name && CFGetTypeID(name) == CFStringGetTypeID() ? static_cast<CFStringRef>(CFRetain(name)) : nullptr;
    refreshParams();
    presetNumber_ = -1;
    // Hosts and views read the parameters again.
    AudioUnitParameter any{unit_, kAUParameterListener_AnyParameter, kAudioUnitScope_Global, 0};
    AUParameterListenerNotify(nullptr, nullptr, &any);
    return noErr;
  }

  // The component bundle (for the Cocoa view's class).
  static CFURLRef bundleUrl() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&AuPlugin::bundleUrl), &info) || !info.dli_fname) return nullptr;
    NSString* bin = [NSString stringWithUTF8String:info.dli_fname];
    NSString* bundle = [[[bin stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    NSURL* url = [[NSURL fileURLWithPath:bundle] retain];  // the caller releases it
    return reinterpret_cast<CFURLRef>(url);
  }

  void detachViews() {
    std::vector<GRIDLINE_AU_VIEW_CLASS*> v = views_;
    for (GRIDLINE_AU_VIEW_CLASS* view : v) [view detachGridline];
    views_.clear();
  }

  Variant variant_;
  std::string type_;
  OSType auType_ = 0, auSubtype_ = 0;
  bool sidechain_ = true;
  std::vector<AUPreset> factory_;
  bool madeFactory_ = false;
  SInt32 presetNumber_ = -1;
  AudioComponentInstance unit_ = nullptr;
  std::unique_ptr<Instance> inst_;
  CFRunLoopTimerRef timer_ = nullptr;
  double sampleRate_ = 44100.0;
  UInt32 maxFrames_ = 1156;
  bool initialized_ = false;
  bool inPlace_ = false;  // outputs are rendered into their own buffers
  bool offline_ = false;
  std::atomic<bool> bypass_{false};
  std::vector<AudioStreamBasicDescription> inFormats_, outFormats_;
  std::vector<Input> inputs_;
  HostCallbackInfo host_{};
  Listener listeners_[kMaxListeners];
  int nListeners_ = 0;
  Notify notify_[kMaxListeners];
  std::atomic<int> nNotify_{0};
  std::unique_ptr<std::atomic<float>[]> params_;  // AU value of each host parameter
  EventRing events_;
  gl::plugin::Event evBuf_[kMaxEvents];
  std::vector<float> inBuf_, outBuf_;
  double cycleTime_ = 0;
  bool haveCycle_ = false;
  uint32_t delivered_ = 0;
  CFStringRef presetName_ = nullptr;
  CFStringRef contextName_ = nullptr;
  std::vector<GRIDLINE_AU_VIEW_CLASS*> views_;
};

}  // namespace

// ---------------------------------------------------------------- Cocoa view

@implementation GRIDLINE_AU_VIEW_CLASS
- (BOOL)isFlipped {
  return YES;
}
- (void)detachGridline {
  if (view) {
    devLog("auview closed frames=%d", view->frames());
    view->detach();
    delete view;
    view = nullptr;
  }
  if (owner) {
    owner->removeView(self);
    owner = nullptr;
  }
}
- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (view) view->setVisible([self window] != nil);
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (view) view->setSize(static_cast<int>(size.width), static_cast<int>(size.height));
}
- (void)dealloc {
  [self detachGridline];
  [super dealloc];
}
@end

@implementation GRIDLINE_AU_FACTORY_CLASS
- (unsigned)interfaceVersion {
  return 0;
}
- (NSString*)description {
  return @"Gridline";
}
- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size {
  AuPlugin* plugin = nullptr;
  UInt32 sz = sizeof plugin;
  if (AudioUnitGetProperty(unit, kGridlineInstanceProperty, kAudioUnitScope_Global, 0, &plugin, &sz) != noErr || !plugin || !plugin->instance())
    return nil;
  if (!gl::plugin::viewSupported(gl::plugin::WindowApi::Cocoa)) return nil;
  int minW = 0, minH = 0, prefW = 0, prefH = 0;
  plugin->instance()->minViewSize(minW, minH);
  plugin->instance()->preferredViewSize(prefW, prefH);
  // (A suite editor opens at its own size.)
  const int w = size.width >= minW && !plugin->instance()->single() ? static_cast<int>(size.width) : prefW;
  const int h = size.height >= minH && !plugin->instance()->single() ? static_cast<int>(size.height) : prefH;
  GRIDLINE_AU_VIEW_CLASS* v = [[[GRIDLINE_AU_VIEW_CLASS alloc] initWithFrame:NSMakeRect(0, 0, w, h)] autorelease];
  std::unique_ptr<gl::plugin::View> gv = gl::plugin::createView(*plugin->instance());
  if (!gv) return nil;
  gv->setSize(w, h);
  GRIDLINE_AU_VIEW_CLASS* weak = v;
  gv->requestResize = [weak, minW, minH](int nw, int nh) {
    [weak setFrameSize:NSMakeSize(std::max(nw, minW), std::max(nh, minH))];
    return true;
  };
  if (!gv->attach(v)) return nil;
  [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];  // hosts may resize it
  v->view = gv.release();
  v->owner = plugin;
  plugin->addView(v);
  devLog("auview opened size=%dx%d", w, h);
  return v;
}
@end

// ---------------------------------------------------------------- factory

#ifndef GRIDLINE_SUITE
extern "C" __attribute__((visibility("default"))) void* GRIDLINE_AU_FACTORY(const AudioComponentDescription* desc) {
  const Variant v = desc && desc->componentSubType == kSubEffect ? Variant::Effect : Variant::Instrument;
  return &(new AuPlugin(v, "", 0, 0, true, false))->iface;
}
#else
extern "C" __attribute__((visibility("default"))) void* GRIDLINE_AU_FACTORY(const AudioComponentDescription* desc) {
  const gl::suite::Member* m = desc ? gl::suite::byAu(desc->componentType, desc->componentSubType) : nullptr;
  if (!m || m->suite != GRIDLINE_SUITE) return nullptr;
  return &(new AuPlugin(m->instrument ? Variant::Instrument : Variant::Effect, m->type, m->auType, m->auSubtype, m->sidechain, m->midiInput))->iface;
}
#endif
#endif

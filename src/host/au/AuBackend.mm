// Gridline — Audio Unit (v2) hosting on macOS (format "au"): instruments
// (aumu), effects (aufx) and music effects (aumf) through AudioToolbox.
// Types: "au:<type>/<subtype>/<manufacturer>" (four-character codes; codes
// that are not plain text are written as 0xXXXXXXXX).
//
// State: "au1:<base64 binary property list>" — the unit's ClassInfo.
// Editor: the unit's Cocoa view (kAudioUnitProperty_CocoaUI), or Apple's
// generic view (CoreAudioKit) when it has none.
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AudioUnitUtilities.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <CoreAudioKit/CoreAudioKit.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../PluginScan.h"

namespace gl::host {

namespace {

thread_local bool tlsAudio = false;


bool parseFourcc(const std::string& s, OSType& out) {
  if (s.size() == 10 && s.compare(0, 2, "0x") == 0) {
    out = static_cast<OSType>(std::strtoul(s.c_str() + 2, nullptr, 16));
    return true;
  }
  if (s.size() != 4) return false;
  out = (static_cast<OSType>(static_cast<uint8_t>(s[0])) << 24) | (static_cast<OSType>(static_cast<uint8_t>(s[1])) << 16) |
        (static_cast<OSType>(static_cast<uint8_t>(s[2])) << 8) | static_cast<OSType>(static_cast<uint8_t>(s[3]));
  return true;
}

std::string keyOf(const AudioComponentDescription& d) { return auKey(d.componentType, d.componentSubType, d.componentManufacturer); }

bool parseKey(const std::string& key, AudioComponentDescription& d) {
  d = AudioComponentDescription{};
  size_t a = key.find('/');
  size_t b = a == std::string::npos ? std::string::npos : key.find('/', a + 1);
  if (b == std::string::npos) return false;
  return parseFourcc(key.substr(0, a), d.componentType) && parseFourcc(key.substr(a + 1, b - a - 1), d.componentSubType) &&
         parseFourcc(key.substr(b + 1), d.componentManufacturer);
}

std::string cfText(CFStringRef s) {
  if (!s) return {};
  char buf[1024];
  if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
  return {};
}

AudioComponent findComponent(const std::string& key) {
  AudioComponentDescription d;
  if (!parseKey(key, d)) return nullptr;
  return AudioComponentFindNext(nullptr, &d);
}

class AuBackend;

class AuView final : public Editor {
 public:
  explicit AuView(AuBackend* b) : b_(b) {}
  ~AuView() override;
  bool size(int& w, int& h) override;
  // Cocoa views that stretch with their superview can be resized.
  bool canResize() const override { return view && ([view autoresizingMask] & (NSViewWidthSizable | NSViewHeightSizable)) != 0; }
  void setSize(int w, int h) override {
    if (view && canResize()) [view setFrameSize:NSMakeSize(w, h)];
  }
  bool attach(void* parent) override;
  AuBackend* b_;
  NSView* view = nil;
  id observer = nil;
};

class AuBackend final : public Backend {
 public:
  static std::unique_ptr<AuBackend> create(const std::string& key, std::string* err) {
    AudioComponent comp = findComponent(key);
    if (!comp) {
      if (err) *err = "component not found: " + key;
      return nullptr;
    }
    AudioComponentInstance unit = nullptr;
    OSStatus st = AudioComponentInstanceNew(comp, &unit);
    if (st != noErr || !unit) {
      if (err) *err = "cannot instantiate the audio unit (" + std::to_string(st) + ")";
      return nullptr;
    }
    std::unique_ptr<AuBackend> b(new AuBackend);
    b->unit_ = unit;
    AudioComponentGetDescription(comp, &b->desc_);
    b->instrument_ = b->desc_.componentType == kAudioUnitType_MusicDevice;
    b->midi_ = b->instrument_ || b->desc_.componentType == kAudioUnitType_MusicEffect;
    b->readBuses();
    b->readParams();
    b->readPresets();
    b->setupCallbacks();
    b->listen();
    return b;
  }
  ~AuBackend() override {
    if (view_) view_->b_ = nullptr;
    if (listener_) AUListenerDispose(listener_);
    deactivate();
    AudioComponentInstanceDispose(unit_);
  }

  AudioUnit unit() const { return unit_; }
  AuView* view_ = nullptr;

  // ---- description ----
  const char* format() const override { return "AU"; }
  const std::vector<ParamInfo>& params() const override { return params_; }
  bool hasEventInput() const override { return midi_; }
  int mainInputChannels() const override { return nIn_ > 0 ? inCh_ : 0; }
  int mainOutputChannels() const override { return nOut_ > 0 ? outCh_ : 0; }
  int sidechainChannels() const override { return hasSc_ ? 2 : 0; }
  int auxOutputCount() const override { return instrument_ ? std::max(0, nOut_ - 1) : 0; }
  bool hasEventOutput() const override { return midiOutputs_ > 0; }
  bool hasBypass() const override { return canBypass_; }
  bool setBypass(bool on) override {
    if (!canBypass_) return false;
    UInt32 v = on ? 1 : 0;
    return AudioUnitSetProperty(unit_, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &v, sizeof v) == noErr;
  }
  bool setOffline(bool offline) override {
    UInt32 v = offline ? 1 : 0;
    return AudioUnitSetProperty(unit_, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &v, sizeof v) == noErr;
  }
  bool popMidiOut(MidiOut& m) override { return midiOut_.pop(m); }

  // ---- UI thread ----
  bool prepare(double sr, int maxBlock) override {
    if (active_ && sr == sampleRate_ && maxBlock == maxBlock_) return true;
    deactivate();
    sampleRate_ = sr;
    maxBlock_ = std::max(1, maxBlock);
    UInt32 frames = static_cast<UInt32>(maxBlock_);
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frames, sizeof frames);
    // Stereo where possible, mono otherwise; non-interleaved 32-bit float.
    auto setFormat = [&](AudioUnitScope scope, UInt32 el, int& ch) {
      for (int want : {2, 1}) {
        AudioStreamBasicDescription f{};
        f.mSampleRate = sr;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
        f.mBytesPerPacket = f.mBytesPerFrame = 4;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = static_cast<UInt32>(want);
        f.mBitsPerChannel = 32;
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, scope, el, &f, sizeof f) == noErr) {
          ch = want;
          return true;
        }
      }
      ch = 0;
      return false;
    };
    if (nIn_ > 0) setFormat(kAudioUnitScope_Input, 0, inCh_);
    int scCh = 0;
    if (hasSc_ && !setFormat(kAudioUnitScope_Input, 1, scCh)) hasSc_ = false;
    outChs_.assign(static_cast<size_t>(std::max(0, nOut_)), 0);
    for (int k = 0; k < nOut_; ++k) setFormat(kAudioUnitScope_Output, static_cast<UInt32>(k), outChs_[static_cast<size_t>(k)]);
    outCh_ = nOut_ > 0 ? outChs_[0] : 0;
    // Buffers: input, sidechain, outputs (2 channels each, maxBlock frames).
    storage_.assign(static_cast<size_t>(4 + 2 * std::max(0, nOut_)) * static_cast<size_t>(maxBlock_), 0.0f);
    if (AudioUnitInitialize(unit_) != noErr) return false;
    active_ = true;
    readLatency();
    return true;
  }
  double sampleRate() const override { return sampleRate_; }
  int maxBlock() const override { return maxBlock_; }
  bool active() const override { return active_; }
  void restart(int32_t flags) override {
    if (flags & kRestartReload) {
      double sr = sampleRate_;
      int mb = maxBlock_;
      deactivate();
      readBuses();
      if (sr > 0) prepare(sr, mb);
    }
    if (flags & kRestartLatency) readLatency();
    if (flags & kRestartParamInfo) readParams();
  }
  int latency() const override { return latency_.load(); }
  int tailSamples() const override {
    Float64 t = 0;
    UInt32 sz = sizeof t;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &t, &sz) != noErr) return 0;
    return static_cast<int>(std::clamp(t * sampleRate_, 0.0, 1e9));
  }
  bool getState(std::string& data) override {
    CFPropertyListRef plist = nullptr;
    UInt32 sz = sizeof plist;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &sz) != noErr || !plist) return false;
    CFDataRef bytes = CFPropertyListCreateData(nullptr, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    CFRelease(plist);
    if (!bytes) return false;
    data = "au1:" + base64Encode(std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), static_cast<size_t>(CFDataGetLength(bytes))));
    CFRelease(bytes);
    return true;
  }
  bool setState(const std::string& data) override {
    std::string bytes;
    if (data.compare(0, 4, "au1:") != 0 || !base64Decode(data.substr(4), bytes)) return false;
    return setClassInfo(bytes);
  }
  // A ClassInfo property list (binary or XML) from elsewhere (FL projects).
  bool setClassInfo(const std::string& bytes) {
    CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size()));
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(d);
    if (!plist) return false;
    OSStatus st = AudioUnitSetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof plist);
    CFRelease(plist);
    notifyAll();
    return st == noErr;
  }
  double paramNormalized(uint32_t id) const override {
    const Range* r = range(id);
    AudioUnitParameterValue v = 0;
    if (!r || AudioUnitGetParameter(unit_, id, kAudioUnitScope_Global, 0, &v) != noErr) return r ? r->defNorm : 0;
    return r->toNorm(v);
  }
  void setParamNormalized(uint32_t id, double v) override {
    (void)v;  // the engine already set the value; tell views about it
    AudioUnitParameter p{unit_, id, kAudioUnitScope_Global, 0};
    AUParameterListenerNotify(listener_, nullptr, &p);
  }
  std::string paramText(uint32_t id, double n) const override {
    const Range* r = range(id);
    if (!r) return {};
    if (!r->labels.empty()) {
      long i = std::lround(r->toPlain(n) - r->min);
      return i >= 0 && static_cast<size_t>(i) < r->labels.size() ? r->labels[static_cast<size_t>(i)] : std::string();
    }
    AudioUnitParameterValue val = static_cast<AudioUnitParameterValue>(r->toPlain(n));
    AudioUnitParameterStringFromValue sv{id, &val, nullptr};
    UInt32 sz = sizeof sv;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &sv, &sz) == noErr && sv.outString) {
      std::string t = cfText(sv.outString);
      CFRelease(sv.outString);
      if (!t.empty()) return r->units.empty() ? t : t + " " + r->units;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, std::fabs(val) >= 100 ? "%.0f" : "%.2f", static_cast<double>(val));
    return r->units.empty() ? std::string(buf) : std::string(buf) + " " + r->units;
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
    if (dirty_.exchange(false)) {
      HostEvent d;
      d.kind = HostEvent::Dirty;
      out.push_back(d);
    }
  }
  int programCount() const override { return static_cast<int>(presets_.size()); }
  std::string programName(int i) const override {
    return i >= 0 && static_cast<size_t>(i) < presets_.size() ? presets_[static_cast<size_t>(i)].second : std::string();
  }
  bool selectProgram(int i) override {
    if (i < 0 || static_cast<size_t>(i) >= presets_.size()) return false;
    CFStringRef name = CFStringCreateWithCString(nullptr, presets_[static_cast<size_t>(i)].second.c_str(), kCFStringEncodingUTF8);
    AUPreset p{presets_[static_cast<size_t>(i)].first, name};
    OSStatus st = AudioUnitSetProperty(unit_, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof p);
    if (st != noErr)  // units that only know the older property
      st = AudioUnitSetProperty(unit_, 24 /* kAudioUnitProperty_CurrentPreset */, kAudioUnitScope_Global, 0, &p, sizeof p);
    CFRelease(name);
    notifyAll();
    return st == noErr;
  }
  bool hasEditor() const override { return true; }  // own Cocoa view or the generic one
  std::unique_ptr<Editor> createEditor() override {
    if (view_) return nullptr;
    auto v = std::make_unique<AuView>(this);
    view_ = v.get();
    return v;
  }

  // ---- audio thread ----
  float* inputChannel(int ch) override { return ch >= 0 && ch < mainInputChannels() ? buf(0, ch) : nullptr; }
  float* sidechainChannel(int ch) override { return ch >= 0 && ch < sidechainChannels() ? buf(1, ch) : nullptr; }
  int outputChannels(int bus) const override {
    return bus >= 0 && bus < nOut_ && (bus == 0 || instrument_) && static_cast<size_t>(bus) < outChs_.size() ? outChs_[static_cast<size_t>(bus)] : 0;
  }
  const float* outputChannel(int bus, int ch) override { return ch >= 0 && ch < outputChannels(bus) ? buf(2 + bus, ch) : nullptr; }
  bool addParamChange(uint32_t id, double v, int offset) override {
    if (pcount_ >= kMaxChanges) return false;
    const Range* r = range(id);
    if (!r) return true;
    changes_[pcount_++] = {id, static_cast<float>(r->toPlain(v)), offset};
    return true;
  }
  void addEvent(const NoteEvent& n) override {
    if (!midi_ || ecount_ >= kMaxEvents) return;
    Midi& m = events2_[ecount_];
    m.offset = n.offset;
    const uint8_t ch = static_cast<uint8_t>(n.channel & 0x0F);
    switch (n.type) {
      case NoteEvent::NoteOn:
        m.s = static_cast<uint8_t>(0x90 | ch);
        m.a = static_cast<uint8_t>(n.key & 0x7F);
        m.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), 1, 127));
        break;
      case NoteEvent::NoteOff:
        m.s = static_cast<uint8_t>(0x80 | ch);
        m.a = static_cast<uint8_t>(n.key & 0x7F);
        m.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), 0, 127));
        break;
      case NoteEvent::Tuning:
        return;
      case NoteEvent::Midi:
        m.s = n.midi[0];
        m.a = n.midi[1];
        m.b = n.midi[2];
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
      for (int b = 0; b < nOut_; ++b)
        for (int c = 0; c < 2; ++c) std::memset(buf(2 + b, c), 0, sizeof(float) * static_cast<size_t>(std::max(0, std::min(n, maxBlock_))));
      clearInput();
      return false;
    }
    tlsAudio = true;
    transport_ = t;
    midiOut_.clear();
    // Sample-accurate where the unit supports it (scheduled immediate events
    // carry their buffer offset); others apply them at the block start.
    for (int i = 0; i < pcount_; ++i) {
      AudioUnitParameterEvent pe{};
      pe.scope = kAudioUnitScope_Global;
      pe.element = 0;
      pe.parameter = changes_[i].id;
      pe.eventType = kParameterEvent_Immediate;
      pe.eventValues.immediate.bufferOffset = static_cast<UInt32>(std::clamp(changes_[i].offset, 0, n - 1));
      pe.eventValues.immediate.value = changes_[i].value;
      if (AudioUnitScheduleParameters(unit_, &pe, 1) != noErr)
        AudioUnitSetParameter(unit_, changes_[i].id, kAudioUnitScope_Global, 0, changes_[i].value, 0);
    }
    pcount_ = 0;
    for (int i = 0; i < ecount_; ++i)
      MusicDeviceMIDIEvent(unit_, events2_[i].s, events2_[i].a, events2_[i].b, static_cast<UInt32>(std::max(0, events2_[i].offset)));
    ecount_ = 0;
    frames_ = n;
    AudioTimeStamp ts{};
    ts.mSampleTime = static_cast<Float64>(t.continuousSamples);
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    bool ok = true;
    for (int b = 0; b < nOut_ && (b == 0 || instrument_); ++b) {
      const int ch = outChs_[static_cast<size_t>(b)];
      abl_.list.mNumberBuffers = static_cast<UInt32>(std::max(1, ch));
      for (int c = 0; c < std::max(1, ch); ++c) {
        abl_.list.mBuffers[c].mNumberChannels = 1;
        abl_.list.mBuffers[c].mDataByteSize = static_cast<UInt32>(n) * 4;
        abl_.list.mBuffers[c].mData = buf(2 + b, c);
      }
      AudioUnitRenderActionFlags flags = 0;
      if (AudioUnitRender(unit_, &flags, &ts, static_cast<UInt32>(b), static_cast<UInt32>(n), &abl_.list) != noErr) {
        for (int c = 0; c < 2; ++c) std::memset(buf(2 + b, c), 0, sizeof(float) * static_cast<size_t>(n));
        if (b == 0) ok = false;
      } else {
        // Units may hand back their own buffers.
        for (int c = 0; c < std::max(1, ch); ++c)
          if (abl_.list.mBuffers[c].mData && abl_.list.mBuffers[c].mData != buf(2 + b, c))
            std::memcpy(buf(2 + b, c), abl_.list.mBuffers[c].mData, sizeof(float) * static_cast<size_t>(n));
      }
    }
    tlsAudio = false;
    return ok;
  }

  void viewGone() { view_ = nullptr; }

 private:
  struct Range {
    uint32_t id = 0;
    double min = 0, max = 1, defNorm = 0;
    std::string units;
    std::vector<std::string> labels;
    double toNorm(double v) const { return max > min ? std::clamp((v - min) / (max - min), 0.0, 1.0) : 0.0; }
    double toPlain(double n) const { return min + std::clamp(n, 0.0, 1.0) * (max - min); }
  };
  struct Change {
    uint32_t id;
    float value;
    int offset;
  };
  struct Midi {
    uint8_t s = 0, a = 0, b = 0;
    int offset = 0;
  };
  static constexpr int kMaxChanges = 2048, kMaxEvents = 1024, kMaxBufs = 8;
  AuBackend() : changes_(new Change[kMaxChanges]), events2_(new Midi[kMaxEvents]) {}

  float* buf(int slot, int ch) { return storage_.data() + (static_cast<size_t>(slot) * 2 + static_cast<size_t>(std::clamp(ch, 0, 1))) * static_cast<size_t>(maxBlock_); }

  void readBuses() {
    UInt32 n = 0, sz = sizeof n;
    nIn_ = AudioUnitGetProperty(unit_, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &n, &sz) == noErr ? static_cast<int>(n) : 0;
    sz = sizeof n;
    nOut_ = AudioUnitGetProperty(unit_, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &n, &sz) == noErr ? static_cast<int>(n) : 1;
    nOut_ = std::clamp(nOut_, 0, 16);
    hasSc_ = !instrument_ && nIn_ >= 2;
    inCh_ = nIn_ > 0 ? 2 : 0;
    outCh_ = nOut_ > 0 ? 2 : 0;
    outChs_.assign(static_cast<size_t>(nOut_), 2);
  }
  void readParams() {
    params_.clear();
    ranges_.clear();
    UInt32 sz = 0;
    Boolean w = false;
    if (AudioUnitGetPropertyInfo(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, &w) == noErr && sz > 0) {
      std::vector<AudioUnitParameterID> ids(sz / sizeof(AudioUnitParameterID));
      if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &sz) == noErr) {
        for (AudioUnitParameterID id : ids) {
          AudioUnitParameterInfo pi{};
          UInt32 isz = sizeof pi;
          if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &pi, &isz) != noErr) continue;
          Range r;
          r.id = id;
          r.min = pi.minValue;
          r.max = pi.maxValue;
          r.defNorm = r.toNorm(pi.defaultValue);
          ParamInfo p;
          p.id = id;
          if ((pi.flags & kAudioUnitParameterFlag_HasCFNameString) && pi.cfNameString) {
            p.title = cfText(pi.cfNameString);
            if (pi.flags & kAudioUnitParameterFlag_CFNameRelease) CFRelease(pi.cfNameString);
          } else {
            p.title = std::string(pi.name, strnlen(pi.name, sizeof pi.name));
          }
          if (pi.unit == kAudioUnitParameterUnit_CustomUnit && pi.unitName) r.units = cfText(pi.unitName);
          else if (pi.unit == kAudioUnitParameterUnit_Hertz) r.units = "Hz";
          else if (pi.unit == kAudioUnitParameterUnit_Decibels) r.units = "dB";
          else if (pi.unit == kAudioUnitParameterUnit_Seconds) r.units = "s";
          else if (pi.unit == kAudioUnitParameterUnit_Milliseconds) r.units = "ms";
          else if (pi.unit == kAudioUnitParameterUnit_Percent || pi.unit == kAudioUnitParameterUnit_EqualPowerCrossfade) r.units = "%";
          else if (pi.unit == kAudioUnitParameterUnit_Cents) r.units = "ct";
          else if (pi.unit == kAudioUnitParameterUnit_RelativeSemiTones) r.units = "st";
          p.units = r.units;
          p.def = r.defNorm;
          if (pi.unit == kAudioUnitParameterUnit_Boolean) p.stepCount = 1;
          if (pi.unit == kAudioUnitParameterUnit_Indexed && r.max > r.min && r.max - r.min <= 4096) {
            p.stepCount = static_cast<int>(std::lround(r.max - r.min));
            CFArrayRef names = nullptr;
            UInt32 nsz = sizeof names;
            if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &names, &nsz) == noErr && names) {
              for (CFIndex k = 0; k < CFArrayGetCount(names); ++k) r.labels.push_back(cfText(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, k))));
              CFRelease(names);
            }
          }
          p.flags = kParamAutomatable;
          if (!(pi.flags & kAudioUnitParameterFlag_IsWritable)) p.flags |= kParamReadOnly;
          if (pi.flags & kAudioUnitParameterFlag_NonRealTime) p.flags &= ~kParamAutomatable;
          params_.push_back(std::move(p));
          ranges_.push_back(std::move(r));
        }
      }
    }
    std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.id < b.id; });
    indexParams();
  }
  const Range* range(uint32_t id) const {
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), id, [](const Range& r, uint32_t v) { return r.id < v; });
    return it != ranges_.end() && it->id == id ? &*it : nullptr;
  }
  void readPresets() {
    presets_.clear();
    CFArrayRef arr = nullptr;
    UInt32 sz = sizeof arr;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &arr, &sz) != noErr || !arr) return;
    for (CFIndex i = 0; i < std::min<CFIndex>(CFArrayGetCount(arr), 1024); ++i) {
      const AUPreset* p = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(arr, i));
      if (p) presets_.push_back({p->presetNumber, cfText(p->presetName)});
    }
    CFRelease(arr);
  }
  void readLatency() {
    Float64 s = 0;
    UInt32 sz = sizeof s;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &s, &sz) == noErr)
      latency_.store(static_cast<int>(std::lround(std::clamp(s * sampleRate_, 0.0, 1e8))));
  }
  void deactivate() {
    if (!active_) return;
    AudioUnitUninitialize(unit_);
    active_ = false;
  }
  void notifyAll() {
    AudioUnitParameter p{unit_, kAUParameterListener_AnyParameter, kAudioUnitScope_Global, 0};
    AUParameterListenerNotify(listener_, nullptr, &p);
  }

  // Input pulls and transport for the unit.
  static OSStatus inputProc(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<AuBackend*>(ref);
    const int slot = bus == 0 ? 0 : 1;
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
      const float* src = self->buf(slot, static_cast<int>(c));
      if (io->mBuffers[c].mData)
        std::memcpy(io->mBuffers[c].mData, src, sizeof(float) * std::min<size_t>(frames, io->mBuffers[c].mDataByteSize / 4));
      else
        io->mBuffers[c].mData = const_cast<float*>(src);
    }
    return noErr;
  }
  static OSStatus beatAndTempo(void* ref, Float64* beat, Float64* tempo) {
    auto* self = static_cast<AuBackend*>(ref);
    if (beat) *beat = self->transport_.projectQn;
    if (tempo) *tempo = self->transport_.tempo;
    return noErr;
  }
  static OSStatus musicalTime(void* ref, UInt32* deltaNext, Float32* num, UInt32* den, Float64* downBeat) {
    auto* self = static_cast<AuBackend*>(ref);
    const Transport& t = self->transport_;
    if (deltaNext) {  // samples to the next beat
      const double beatQn = 4.0 / std::max(1, t.tsDen);
      const double next = std::ceil(t.projectQn / beatQn - 1e-9) * beatQn;
      *deltaNext = static_cast<UInt32>(std::max(0L, std::lround((next - t.projectQn) * 60.0 / std::max(1.0, t.tempo) * t.sampleRate)));
    }
    if (num) *num = static_cast<Float32>(t.tsNum);
    if (den) *den = static_cast<UInt32>(t.tsDen);
    if (downBeat) *downBeat = t.barQn;
    return noErr;
  }
  static OSStatus transportState(void* ref, Boolean* playing, Boolean* changed, Float64* sample, Boolean* cycling, Float64* cs, Float64* ce) {
    return transportState2(ref, playing, nullptr, changed, sample, cycling, cs, ce);
  }
  static OSStatus transportState2(void* ref, Boolean* playing, Boolean* recording, Boolean* changed, Float64* sample, Boolean* cycling,
                                  Float64* cs, Float64* ce) {
    auto* self = static_cast<AuBackend*>(ref);
    const Transport& t = self->transport_;
    if (playing) *playing = t.playing;
    if (recording) *recording = t.recording;
    if (changed) *changed = t.changed;
    if (sample) *sample = static_cast<Float64>(t.projectSamples);
    if (cycling) *cycling = t.looping;
    if (cs) *cs = t.loopStartQn;
    if (ce) *ce = t.loopEndQn;
    return noErr;
  }
  // MIDI the unit sends (music effects, MIDI processors), during AudioUnitRender.
  static OSStatus midiOutput(void* ref, const AudioTimeStamp*, UInt32, const struct MIDIPacketList* list) {
    auto* self = static_cast<AuBackend*>(ref);
    if (!list || !tlsAudio) return noErr;
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets && i < 256; ++i) {
      for (UInt16 k = 0; k + 1 < p->length;) {
        const uint8_t st = p->data[k];
        if (st < 0x80 || st >= 0xF0) {
          ++k;
          continue;
        }
        const int len = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
        if (k + len > p->length) break;
        self->midiOut_.push(0, st, p->data[k + 1], len == 3 ? p->data[k + 2] : 0);
        k = static_cast<UInt16>(k + len);
      }
      p = MIDIPacketNext(p);
    }
    return noErr;
  }
  void setupCallbacks() {
    AURenderCallbackStruct cb{&AuBackend::inputProc, this};
    if (nIn_ > 0) AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (hasSc_) AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 1, &cb, sizeof cb);
    HostCallbackInfo hc{};
    hc.hostUserData = this;
    hc.beatAndTempoProc = &AuBackend::beatAndTempo;
    hc.musicalTimeLocationProc = &AuBackend::musicalTime;
    hc.transportStateProc = &AuBackend::transportState;
    hc.transportStateProc2 = &AuBackend::transportState2;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &hc, sizeof hc);
    // MIDI output (units that list output ports).
    CFArrayRef names = nullptr;
    UInt32 sz = sizeof names;
    midiOutputs_ = 0;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_MIDIOutputCallbackInfo, kAudioUnitScope_Global, 0, &names, &sz) == noErr && names) {
      midiOutputs_ = static_cast<int>(CFArrayGetCount(names));
      CFRelease(names);
    }
    if (midiOutputs_ > 0) {
      AUMIDIOutputCallbackStruct mo{&AuBackend::midiOutput, this};
      AudioUnitSetProperty(unit_, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, &mo, sizeof mo);
    }
    // Bypass: effects whose bypass property is writable.
    Boolean writable = false;
    UInt32 bsz = 0;
    canBypass_ = !instrument_ &&
                 AudioUnitGetPropertyInfo(unit_, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &bsz, &writable) == noErr && writable;
  }
  // Edits in the unit's view and property changes, delivered on a private queue.
  void listen() {
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    AuBackend* self = this;
    AUEventListenerCreateWithDispatchQueue(&listener_, 0.01f, 0.005f, q, ^(void*, const AudioUnitEvent* e, UInt64, AudioUnitParameterValue v) {
      HostEvent h;
      switch (e->mEventType) {
        case kAudioUnitEvent_ParameterValueChange: {
          const Range* r = self->range(e->mArgument.mParameter.mParameterID);
          if (!r || e->mArgument.mParameter.mParameterID == kAUParameterListener_AnyParameter) return;
          h.kind = HostEvent::PerformEdit;
          h.id = e->mArgument.mParameter.mParameterID;
          h.value = r->toNorm(v);
          break;
        }
        case kAudioUnitEvent_BeginParameterChangeGesture:
          h.kind = HostEvent::BeginEdit;
          h.id = e->mArgument.mParameter.mParameterID;
          break;
        case kAudioUnitEvent_EndParameterChangeGesture:
          h.kind = HostEvent::EndEdit;
          h.id = e->mArgument.mParameter.mParameterID;
          break;
        case kAudioUnitEvent_PropertyChange: {
          const AudioUnitPropertyID pid = e->mArgument.mProperty.mPropertyID;
          if (pid == kAudioUnitProperty_Latency) self->restartFlags_ |= kRestartLatency;
          else if (pid == kAudioUnitProperty_ParameterList || pid == kAudioUnitProperty_ParameterInfo) self->restartFlags_ |= kRestartParamInfo;
          else {
            self->restartFlags_ |= kRestartParamValues;
            self->dirty_.store(true);
          }
          return;
        }
        default:
          return;
      }
      self->events_.push(h);
    });
    if (!listener_) return;
    for (const auto& r : ranges_)
      for (AudioUnitEventType t : {kAudioUnitEvent_ParameterValueChange, kAudioUnitEvent_BeginParameterChangeGesture, kAudioUnitEvent_EndParameterChangeGesture}) {
        AudioUnitEvent ev{};
        ev.mEventType = t;
        ev.mArgument.mParameter = AudioUnitParameter{unit_, r.id, kAudioUnitScope_Global, 0};
        AUEventListenerAddEventType(listener_, nullptr, &ev);
      }
    for (AudioUnitPropertyID pid : {kAudioUnitProperty_Latency, kAudioUnitProperty_ParameterList, kAudioUnitProperty_PresentPreset}) {
      AudioUnitEvent ev{};
      ev.mEventType = kAudioUnitEvent_PropertyChange;
      ev.mArgument.mProperty = AudioUnitProperty{unit_, pid, kAudioUnitScope_Global, 0};
      AUEventListenerAddEventType(listener_, nullptr, &ev);
    }
  }

  AudioUnit unit_ = nullptr;
  AudioComponentDescription desc_{};
  bool instrument_ = false, midi_ = false;
  int nIn_ = 0, nOut_ = 1, inCh_ = 2, outCh_ = 2;
  bool hasSc_ = false;
  int midiOutputs_ = 0;
  bool canBypass_ = false;
  MidiOutBuffer midiOut_;
  std::vector<int> outChs_;
  std::vector<ParamInfo> params_;
  std::vector<Range> ranges_;
  std::vector<std::pair<SInt32, std::string>> presets_;
  double sampleRate_ = 0;
  int maxBlock_ = 0;
  bool active_ = false;
  std::atomic<int> latency_{0};
  std::vector<float> storage_;
  std::unique_ptr<Change[]> changes_;
  int pcount_ = 0;
  std::unique_ptr<Midi[]> events2_;
  int ecount_ = 0;
  int frames_ = 0;
  Transport transport_;
  struct {
    AudioBufferList list;
    AudioBuffer more[kMaxBufs];
  } abl_{};
  AUEventListenerRef listener_ = nullptr;
  HostEventQueue events_;
  std::atomic<int32_t> restartFlags_{0};
  std::atomic<bool> dirty_{false};
};

AuView::~AuView() {
  if (observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
    [observer release];
  }
  if (view) {
    [view removeFromSuperview];
    [view release];
  }
  if (b_) b_->viewGone();
}

bool AuView::size(int& w, int& h) {
  if (!b_) return false;
  if (!view) {
    AudioUnit unit = b_->unit();
    UInt32 sz = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &sz, &writable) == noErr && sz >= sizeof(AudioUnitCocoaViewInfo)) {
      std::vector<uint8_t> mem(sz);
      auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*>(mem.data());
      if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &sz) == noErr) {
        NSURL* url = (NSURL*)info->mCocoaAUViewBundleLocation;
        NSString* cls = (NSString*)info->mCocoaAUViewClass[0];
        NSBundle* bundle = url ? [NSBundle bundleWithURL:url] : nil;
        Class c = bundle && cls ? [bundle classNamed:cls] : nil;
        if (c && [c conformsToProtocol:@protocol(AUCocoaUIBase)]) {
          NSObject<AUCocoaUIBase>* factory = [[c alloc] init];
          NSView* v = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(800, 600)];
          if (v) view = [v retain];
          [factory release];
        }
        if (url) CFRelease(info->mCocoaAUViewBundleLocation);
        const size_t classes = (sz - offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass)) / sizeof(CFStringRef);
        for (size_t k = 0; k < classes; ++k)
          if (info->mCocoaAUViewClass[k]) CFRelease(info->mCocoaAUViewClass[k]);
      }
    }
    if (!view) view = [[AUGenericView alloc] initWithAudioUnit:unit];
  }
  if (!view) return false;
  NSSize s = [view frame].size;
  w = std::max(64, static_cast<int>(s.width));
  h = std::max(48, static_cast<int>(s.height));
  return true;
}

bool AuView::attach(void* parent) {
  int w = 0, h = 0;
  if (!parent || !size(w, h)) return false;
  NSView* p = (NSView*)parent;
  [view setFrameOrigin:NSMakePoint(0, 0)];
  [p addSubview:view];
  [view setPostsFrameChangedNotifications:YES];
  AuView* self = this;
  observer = [[[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
                                                                object:view
                                                                 queue:nil
                                                            usingBlock:^(NSNotification*) {
                                                              NSSize s = [self->view frame].size;
                                                              if (self->requestResize) self->requestResize(static_cast<int>(s.width), static_cast<int>(s.height));
                                                            }] retain];
  return true;
}

// ---------------------------------------------------------------- format

std::vector<std::string> auPaths() {
  std::vector<std::string> v;
  if (const char* home = std::getenv("HOME")) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/Components");
  v.push_back("/Library/Audio/Plug-Ins/Components");
  return v;
}

// Every installed instrument / effect / music effect (the system registry).
std::vector<std::string> auDiscover(const std::vector<std::string>&) {
  std::vector<std::string> out;
  for (OSType type : {kAudioUnitType_MusicDevice, kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
    AudioComponentDescription d{};
    d.componentType = type;
    AudioComponent c = nullptr;
    while ((c = AudioComponentFindNext(c, &d)) != nullptr) {
      AudioComponentDescription cd{};
      if (AudioComponentGetDescription(c, &cd) == noErr) out.push_back(keyOf(cd));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

int64_t auStamp(const std::string& key) {
  AudioComponent c = findComponent(key);
  UInt32 v = 0;
  if (!c || AudioComponentGetVersion(c, &v) != noErr) return 0;
  return v;
}

ScanResult auScan(const std::string& key) {
  ScanResult r;
  r.format = "au";
  r.bundle = key;
  r.mtime = auStamp(key);
  AudioComponent comp = findComponent(key);
  if (!comp) {
    r.error = "component not registered";
    return r;
  }
  std::string err;
  auto b = AuBackend::create(key, &err);
  if (!b) {
    r.error = err;
    return r;
  }
  ScannedClass c;
  c.uid = key;
  CFStringRef full = nullptr;
  if (AudioComponentCopyName(comp, &full) == noErr && full) {
    std::string n = cfText(full);
    CFRelease(full);
    size_t colon = n.find(": ");
    c.vendor = colon == std::string::npos ? std::string() : n.substr(0, colon);
    c.name = colon == std::string::npos ? n : n.substr(colon + 2);
  }
  if (c.name.empty()) c.name = key;
  UInt32 v = 0;
  if (AudioComponentGetVersion(comp, &v) == noErr) c.version = std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
  AudioComponentDescription d{};
  AudioComponentGetDescription(comp, &d);
  c.instrument = d.componentType == kAudioUnitType_MusicDevice;
  c.subCategories = c.instrument ? "Instrument" : (d.componentType == kAudioUnitType_MusicEffect ? "Fx|Music effect" : "Fx");
  c.sdkVersion = "AU v2";
  c.eventInput = b->hasEventInput();
  c.eventOutput = b->hasEventOutput();
  if (b->prepare(48000.0, 512)) {
    c.inputChannels = b->mainInputChannels();
    c.outputChannels = b->mainOutputChannels();
    c.sidechainChannels = b->sidechainChannels();
    c.latency = b->latency();
    for (int k = 0; k < b->auxOutputCount(); ++k) c.auxOutputs.push_back(b->auxOutputName(k));
  }
  for (int k = 0; k < b->programCount(); ++k) c.programs.push_back(b->programName(k));
  for (const ParamInfo& p : b->params()) {
    ScannedParam sp;
    sp.id = p.id;
    sp.title = p.title;
    sp.units = p.units;
    sp.stepCount = p.stepCount;
    sp.def = p.def;
    sp.flags = p.flags;
    if (p.stepCount >= 1 && p.stepCount <= 32 && p.exposed())
      for (int k = 0; k <= p.stepCount; ++k) sp.labels.push_back(b->paramText(p.id, static_cast<double>(k) / p.stepCount));
    c.params.push_back(std::move(sp));
  }
  r.classes.push_back(std::move(c));
  r.ok = true;
  return r;
}

std::unique_ptr<Backend> auCreate(const std::string& key, const std::string& uid, std::string* err) {
  (void)uid;  // the entry is the component key
  return AuBackend::create(key, err);
}

}  // namespace

Format auFormat() {
  Format f{};
  f.id = "au";
  f.label = "AU";
  f.defaultPaths = auPaths;
  f.discover = auDiscover;
  f.scanInProcess = auScan;
  f.create = auCreate;
  f.mtime = auStamp;
  return f;
}

}  // namespace gl::host

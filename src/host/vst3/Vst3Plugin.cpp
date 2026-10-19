#include "Vst3Plugin.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"

namespace gl::vst3 {

// IComponentHandler given to the controller: forwards to the owning Plugin.
struct Plugin::Handler final : HostObject<sv::IComponentHandler, sv::IComponentHandler2> {
  explicit Handler(Plugin* p) : plugin(p) {}
  Plugin* plugin;
  sb::tresult PLUGIN_API beginEdit(sv::ParamID id) override {
    plugin->pushHostEvent({HostEvent::BeginEdit, id, 0, 0});
    return sb::kResultOk;
  }
  sb::tresult PLUGIN_API performEdit(sv::ParamID id, sv::ParamValue v) override {
    plugin->pushHostEvent({HostEvent::PerformEdit, id, v, 0});
    return sb::kResultOk;
  }
  sb::tresult PLUGIN_API endEdit(sv::ParamID id) override {
    plugin->pushHostEvent({HostEvent::EndEdit, id, 0, 0});
    return sb::kResultOk;
  }
  sb::tresult PLUGIN_API restartComponent(sb::int32 flags) override {
    plugin->pushHostEvent({HostEvent::Restart, 0, 0, flags});
    return sb::kResultOk;
  }
  sb::tresult PLUGIN_API setDirty(sb::TBool state) override {
    if (state) plugin->pushHostEvent({HostEvent::Dirty, 0, 0, 0});
    return sb::kResultOk;
  }
  sb::tresult PLUGIN_API requestOpenEditor(sb::FIDString) override { return sb::kResultFalse; }
  sb::tresult PLUGIN_API startGroupEdit() override { return sb::kResultOk; }
  sb::tresult PLUGIN_API finishGroupEdit() override { return sb::kResultOk; }
};

std::unique_ptr<Plugin> Plugin::create(std::shared_ptr<Module> module, const std::string& uid, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return std::unique_ptr<Plugin>();
  };
  if (!module) return fail("no module");
  const ClassInfo* ci = module->findClass(uid);
  if (!ci) return fail("class not found in " + module->path());
  std::unique_ptr<Plugin> p(new Plugin);
  p->module_ = module;
  p->info_ = *ci;
  p->component_ = module->create<sv::IComponent>(uid);
  if (!p->component_) return fail("cannot create the component");
  if (p->component_->initialize(static_cast<sv::IHostApplication*>(hostContext())) != sb::kResultOk) {
    p->component_.reset();
    return fail("component initialize failed");
  }
  p->processor_ = Ptr<sv::IAudioProcessor>::query<sv::IAudioProcessor>(p->component_.get());
  if (!p->processor_) {
    p->component_->terminate();
    p->component_.reset();
    return fail("component has no audio processor");
  }
  // Controller: the component itself, or a separate class.
  p->controller_ = Ptr<sv::IEditController>::query<sv::IEditController>(p->component_.get());
  if (p->controller_) {
    p->singleComponent_ = true;
  } else {
    sb::TUID cid{};
    if (p->component_->getControllerClassId(cid) == sb::kResultOk) {
      p->controller_ = module->create<sv::IEditController>(uidToHex(cid));
      if (p->controller_ && p->controller_->initialize(static_cast<sv::IHostApplication*>(hostContext())) != sb::kResultOk) p->controller_.reset();
    }
  }
  p->handler_ = std::make_unique<Handler>(p.get());
  if (p->controller_) {
    p->controller_->setComponentHandler(p->handler_.get());
    if (!p->singleComponent_) {
      p->compCP_ = Ptr<sv::IConnectionPoint>::query<sv::IConnectionPoint>(p->component_.get());
      p->ctrlCP_ = Ptr<sv::IConnectionPoint>::query<sv::IConnectionPoint>(p->controller_.get());
      if (p->compCP_ && p->ctrlCP_) {
        p->compCP_->connect(p->ctrlCP_.get());
        p->ctrlCP_->connect(p->compCP_.get());
      }
    }
    // Synchronize the controller with the component's initial state.
    MemoryStream s;
    if (p->component_->getState(&s) == sb::kResultOk) {
      s.rewind();
      p->controller_->setComponentState(&s);
    }
    if (auto ne = Ptr<sv::INoteExpressionController>::query<sv::INoteExpressionController>(p->controller_.get())) {
      sb::int32 n = ne->getNoteExpressionCount(0, 0);
      for (sb::int32 i = 0; i < n; ++i) {
        sv::NoteExpressionTypeInfo ti{};
        if (ne->getNoteExpressionInfo(0, 0, i, ti) != sb::kResultOk) continue;
        if (ti.typeId == sv::kTuningTypeID) p->tuningExpr_ = true;
        if (p->exprTypes_.size() < 64) p->exprTypes_.push_back(ti.typeId);
      }
    }
  }
  p->eventInputs_ = p->component_->getBusCount(sv::kEvent, sv::kInput);
  p->eventOutputs_ = p->component_->getBusCount(sv::kEvent, sv::kOutput);
  p->readParams();
  p->readMidiMapping();
  p->readPrograms();
  p->outParams_.resize(64);
  return p;
}

void Plugin::readMidiMapping() {
  midiMap_.clear();
  if (!controller_) return;
  auto mm = Ptr<sv::IMidiMapping>::query<sv::IMidiMapping>(controller_.get());
  if (!mm) return;
  midiMap_.assign(16 * kMidiCtrls, -1);
  bool any = false;
  for (int ch = 0; ch < 16; ++ch)
    for (int c = 0; c < kMidiCtrls; ++c) {
      sv::ParamID id = 0;
      if (mm->getMidiControllerAssignment(0, static_cast<sb::int16>(ch), static_cast<sv::CtrlNumber>(c), id) == sb::kResultTrue) {
        midiMap_[static_cast<size_t>(ch * kMidiCtrls + c)] = id;
        any = true;
      }
    }
  if (!any) midiMap_.clear();
}

void Plugin::readPrograms() {
  programs_.clear();
  programParam_ = -1;
  if (!controller_) return;
  for (const auto& p : params_)
    if (p.flags & sv::ParameterInfo::kIsProgramChange) {
      programParam_ = p.id;
      break;
    }
  auto ui = Ptr<sv::IUnitInfo>::query<sv::IUnitInfo>(controller_.get());
  if (!ui) return;
  // The root unit's list (or the first list).
  sv::ProgramListID list = sv::kNoProgramListId;
  for (sb::int32 u = 0; u < ui->getUnitCount(); ++u) {
    sv::UnitInfo info{};
    if (ui->getUnitInfo(u, info) == sb::kResultOk && info.id == sv::kRootUnitId) list = info.programListId;
  }
  sb::int32 count = 0;
  for (sb::int32 l = 0; l < ui->getProgramListCount(); ++l) {
    sv::ProgramListInfo li{};
    if (ui->getProgramListInfo(l, li) != sb::kResultOk) continue;
    if (list == sv::kNoProgramListId || li.id == list) {
      list = li.id;
      count = li.programCount;
      break;
    }
  }
  for (sb::int32 i = 0; i < std::min<sb::int32>(count, 1024); ++i) {
    sv::String128 name{};
    programs_.push_back(ui->getProgramName(list, i, name) == sb::kResultOk ? toUtf8(name) : "Program " + std::to_string(i + 1));
  }
  if (programParam_ < 0) programs_.clear();  // no way to select them
}

std::string Plugin::outputBusName(int bus) const {
  sv::BusInfo bi{};
  if (!component_ || component_->getBusInfo(sv::kAudio, sv::kOutput, bus, bi) != sb::kResultOk) return {};
  return toUtf8(bi.name);
}

void Plugin::readParams() {
  params_.clear();
  paramLookup_.clear();
  bypassId_ = -1;
  if (!controller_) return;
  sb::int32 n = controller_->getParameterCount();
  for (sb::int32 i = 0; i < n; ++i) {
    sv::ParameterInfo pi{};
    if (controller_->getParameterInfo(i, pi) != sb::kResultOk) continue;
    ParamInfo p;
    p.id = pi.id;
    p.title = toUtf8(pi.title);
    p.units = toUtf8(pi.units);
    p.stepCount = pi.stepCount;
    p.def = pi.defaultNormalizedValue;
    p.flags = pi.flags;
    if (pi.flags & sv::ParameterInfo::kIsBypass) bypassId_ = pi.id;
    paramLookup_.push_back({p.id, static_cast<int>(params_.size())});
    params_.push_back(std::move(p));
  }
  std::sort(paramLookup_.begin(), paramLookup_.end());
}

int Plugin::paramIndex(uint32_t id) const {
  auto it = std::lower_bound(paramLookup_.begin(), paramLookup_.end(), std::make_pair(id, -1));
  return it != paramLookup_.end() && it->first == id ? it->second : -1;
}

Plugin::~Plugin() {
  if (editorHost_) editorHost_->pluginGoingAway();
  deactivate();
  if (compCP_ && ctrlCP_) {
    compCP_->disconnect(ctrlCP_.get());
    ctrlCP_->disconnect(compCP_.get());
  }
  compCP_.reset();
  ctrlCP_.reset();
  if (controller_) {
    controller_->setComponentHandler(nullptr);
    if (!singleComponent_) controller_->terminate();
  }
  controller_.reset();
  processor_.reset();
  if (component_) component_->terminate();
  component_.reset();
}

void Plugin::deactivate() {
  suspended_.store(true);
  while (busy_.load()) std::this_thread::yield();
  if (processingOn_ && processor_) processor_->setProcessing(false);
  processingOn_ = false;
  if (active_ && component_) component_->setActive(false);
  active_ = false;
}

bool Plugin::setupBuses() {
  using namespace sv;
  const int nIn = component_->getBusCount(kAudio, kInput), nOut = component_->getBusCount(kAudio, kOutput);
  mainIn_ = mainOut_ = scBus_ = -1;
  std::vector<SpeakerArrangement> inArr(static_cast<size_t>(std::max(0, nIn))), outArr(static_cast<size_t>(std::max(0, nOut)));
  for (int i = 0; i < nIn; ++i) {
    BusInfo bi{};
    component_->getBusInfo(kAudio, kInput, i, bi);
    SpeakerArrangement a = 0;
    processor_->getBusArrangement(kInput, i, a);
    if (bi.busType == kMain && mainIn_ < 0) {
      mainIn_ = i;
      a = SpeakerArr::kStereo;
    } else if (bi.busType == kAux && scBus_ < 0) {
      scBus_ = i;
      a = SpeakerArr::kStereo;
    }
    inArr[static_cast<size_t>(i)] = a;
  }
  for (int i = 0; i < nOut; ++i) {
    BusInfo bi{};
    component_->getBusInfo(kAudio, kOutput, i, bi);
    SpeakerArrangement a = 0;
    processor_->getBusArrangement(kOutput, i, a);
    if (bi.busType == kMain && mainOut_ < 0) {
      mainOut_ = i;
      a = SpeakerArr::kStereo;
    }
    outArr[static_cast<size_t>(i)] = a;
  }
  if (mainOut_ < 0 && nOut > 0) mainOut_ = 0;
  if (processor_->setBusArrangements(inArr.data(), nIn, outArr.data(), nOut) != sb::kResultTrue) {
    // Second try: stereo main buses, the sidechain as the plugin has it (mono keys).
    std::vector<SpeakerArrangement> in2 = inArr;
    if (scBus_ >= 0) processor_->getBusArrangement(kInput, scBus_, in2[static_cast<size_t>(scBus_)]);
    if (scBus_ < 0 || processor_->setBusArrangements(in2.data(), nIn, outArr.data(), nOut) != sb::kResultTrue) {
      // Keep what the plugin supports (e.g. mono effects).
      for (int i = 0; i < nIn; ++i) processor_->getBusArrangement(kInput, i, inArr[static_cast<size_t>(i)]);
      for (int i = 0; i < nOut; ++i) processor_->getBusArrangement(kOutput, i, outArr[static_cast<size_t>(i)]);
    } else {
      inArr = in2;
    }
  }
  inCh_.assign(static_cast<size_t>(std::max(0, nIn)), 0);
  outCh_.assign(static_cast<size_t>(std::max(0, nOut)), 0);
  size_t total = 0;
  for (int i = 0; i < nIn; ++i) total += static_cast<size_t>(inCh_[static_cast<size_t>(i)] = std::min(kMaxChannelsPerBus, SpeakerArr::getChannelCount(inArr[static_cast<size_t>(i)])));
  for (int i = 0; i < nOut; ++i) total += static_cast<size_t>(outCh_[static_cast<size_t>(i)] = std::min(kMaxChannelsPerBus, SpeakerArr::getChannelCount(outArr[static_cast<size_t>(i)])));
  // Bus activation: main buses, the sidechain and the first event input.
  for (int i = 0; i < nIn; ++i) component_->activateBus(kAudio, kInput, i, i == mainIn_ || i == scBus_);
  auxOut_.clear();
  for (int i = 0; i < nOut; ++i) {
    const bool aux = i != mainOut_ && outCh_[static_cast<size_t>(i)] > 0;
    if (aux) auxOut_.push_back(i);
    component_->activateBus(kAudio, kOutput, i, i == mainOut_ || aux);
  }
  const int nEv = component_->getBusCount(kEvent, kInput);
  for (int i = 0; i < nEv; ++i) component_->activateBus(kEvent, kInput, i, i == 0);
  const int nEvOut = component_->getBusCount(kEvent, kOutput);
  for (int i = 0; i < nEvOut; ++i) component_->activateBus(kEvent, kOutput, i, i == 0);

  storage_.assign(total * static_cast<size_t>(maxBlock_), 0.0f);
  float* cursor = storage_.data();
  auto wire = [&](std::vector<int>& ch, std::vector<AudioBusBuffers>& bus, std::vector<std::vector<float*>>& ptr) {
    bus.assign(ch.size(), AudioBusBuffers{});
    ptr.assign(ch.size(), {});
    for (size_t b = 0; b < ch.size(); ++b) {
      ptr[b].resize(static_cast<size_t>(std::max(1, ch[b])), nullptr);
      for (int c = 0; c < ch[b]; ++c) {
        ptr[b][static_cast<size_t>(c)] = cursor;
        cursor += maxBlock_;
      }
      bus[b].numChannels = ch[b];
      bus[b].silenceFlags = 0;
      bus[b].channelBuffers32 = ptr[b].data();
    }
  };
  wire(inCh_, inBus_, inPtr_);
  wire(outCh_, outBus_, outPtr_);
  return true;
}

bool Plugin::prepare(double sampleRate, int maxBlock) {
  if (!component_ || !processor_) return false;
  if (active_ && sampleRate == sampleRate_ && maxBlock == maxBlock_) return true;
  deactivate();
  sampleRate_ = sampleRate;
  maxBlock_ = std::max(1, maxBlock);
  if (processor_->canProcessSampleSize(sv::kSample32) != sb::kResultTrue) return false;
  setupBuses();
  sv::ProcessSetup setup{};
  setup.processMode = offline_ ? sv::kOffline : sv::kRealtime;
  setup.symbolicSampleSize = sv::kSample32;
  setup.maxSamplesPerBlock = maxBlock_;
  setup.sampleRate = sampleRate_;
  if (processor_->setupProcessing(setup) != sb::kResultOk) return false;
  // Enough queues for every parameter changing in one block (capped).
  inParams_.resize(static_cast<int>(std::min<size_t>(std::max<size_t>(params_.size(), 16), 4096)));
  if (component_->setActive(true) != sb::kResultOk) return false;
  active_ = true;
  latency_.store(static_cast<int>(processor_->getLatencySamples()));
  tail_ = static_cast<int>(std::min<sb::uint32>(processor_->getTailSamples(), 1u << 30));
  processor_->setProcessing(true);  // some plugins return kNotImplemented: not an error
  processingOn_ = true;
  suspended_.store(false);
  return true;
}

void Plugin::setOffline(bool offline) {
  if (offline == offline_) return;
  offline_ = offline;
  if (!active_) return;
  double sr = sampleRate_;
  int mb = maxBlock_;
  deactivate();
  prepare(sr, mb);
}

void Plugin::restart(int32_t flags) {
  if (flags & (sv::kReloadComponent | sv::kIoChanged)) {
    double sr = sampleRate_;
    int mb = maxBlock_;
    deactivate();
    if (sr > 0) prepare(sr, mb);
  } else if (flags & sv::kLatencyChanged) {
    // The new latency applies after the component is reactivated.
    double sr = sampleRate_;
    int mb = maxBlock_;
    deactivate();
    if (sr > 0) prepare(sr, mb);
    if (processor_) latency_.store(static_cast<int>(processor_->getLatencySamples()));
  }
  if (flags & (sv::kParamTitlesChanged | sv::kNoteExpressionChanged)) {
    readParams();
    readPrograms();
  }
  if (flags & sv::kMidiCCAssignmentChanged) readMidiMapping();
}

bool Plugin::getState(std::string& component, std::string& controller) {
  component.clear();
  controller.clear();
  if (!component_) return false;
  MemoryStream cs;
  if (component_->getState(&cs) != sb::kResultOk) return false;
  component = cs.data();
  if (controller_ && !singleComponent_) {
    MemoryStream es;
    if (controller_->getState(&es) == sb::kResultOk) controller = es.data();
  }
  return true;
}

bool Plugin::setState(const std::string& component, const std::string& controller) {
  if (!component_) return false;
  bool ok = true;
  if (!component.empty()) {
    MemoryStream cs(component);
    ok = component_->setState(&cs) == sb::kResultOk;
    if (controller_) {
      MemoryStream cs2(component);
      controller_->setComponentState(&cs2);
    }
  }
  if (!controller.empty() && controller_ && !singleComponent_) {
    MemoryStream es(controller);
    controller_->setState(&es);
  }
  return ok;
}

double Plugin::paramNormalized(uint32_t id) const { return controller_ ? controller_->getParamNormalized(id) : 0.0; }

void Plugin::setParamNormalized(uint32_t id, double value) {
  if (controller_) controller_->setParamNormalized(id, value);
}

std::string Plugin::paramText(uint32_t id, double normalized) const {
  if (!controller_) return {};
  sv::String128 s{};
  if (controller_->getParamStringByValue(id, normalized, s) != sb::kResultOk) return {};
  return toUtf8(s);
}

void Plugin::pushHostEvent(const HostEvent& e) {
  hostQueue_.push({static_cast<host::HostEvent::Kind>(e.kind), e.id, e.value, e.flags});
}

void Plugin::drainHostEvents(std::vector<HostEvent>& out) {
  host::HostEvent h;
  while (hostQueue_.pop(h)) out.push_back({static_cast<HostEvent::Kind>(h.kind), h.id, h.value, h.flags});
}

bool Plugin::popOutputValue(OutputValue& v) {
  size_t t = outTail_.load(std::memory_order_relaxed);
  if (t == outHead_.load(std::memory_order_acquire)) return false;
  v = outRing_[t % kOutRing];
  outTail_.store(t + 1, std::memory_order_release);
  return true;
}

bool Plugin::process(int n, const sv::ProcessContext& ctx) {
  busy_.store(true);
  if (suspended_.load() || !active_ || n <= 0 || n > maxBlock_) {
    busy_.store(false);
    for (size_t b = 0; b < outPtr_.size(); ++b)
      for (int c = 0; c < outCh_[b]; ++c) std::memset(outPtr_[b][static_cast<size_t>(c)], 0, sizeof(float) * static_cast<size_t>(std::max(0, std::min(n, maxBlock_))));
    inParams_.clear();
    inEvents_.clear();
    return false;
  }
  sv::ProcessContext pc = ctx;
  sv::ProcessData d{};
  d.processMode = offline_ ? sv::kOffline : sv::kRealtime;
  d.symbolicSampleSize = sv::kSample32;
  d.numSamples = n;
  d.numInputs = static_cast<sb::int32>(inBus_.size());
  d.numOutputs = static_cast<sb::int32>(outBus_.size());
  d.inputs = inBus_.empty() ? nullptr : inBus_.data();
  d.outputs = outBus_.empty() ? nullptr : outBus_.data();
  d.inputParameterChanges = &inParams_;
  outParams_.clear();
  d.outputParameterChanges = &outParams_;
  d.inputEvents = eventInputs_ > 0 ? &inEvents_ : nullptr;
  outEvents_.clear();
  d.outputEvents = eventOutputs_ > 0 ? &outEvents_ : nullptr;
  d.processContext = &pc;
  for (auto& b : outBus_) b.silenceFlags = 0;
  for (auto& b : inBus_) b.silenceFlags = 0;
  midiOut_.clear();
  processor_->process(d);
  // Note / MIDI output (arpeggiators, MIDI effects).
  for (sb::int32 i = 0; i < outEvents_.getEventCount(); ++i) {
    sv::Event e{};
    if (outEvents_.getEvent(i, e) != sb::kResultOk) continue;
    auto vel = [](float v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 127.0f)), 0, 127)); };
    switch (e.type) {
      case sv::Event::kNoteOnEvent:
        midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0x90 | (e.noteOn.channel & 0x0F)), static_cast<uint8_t>(e.noteOn.pitch & 0x7F),
                      std::max<uint8_t>(1, vel(e.noteOn.velocity)));
        break;
      case sv::Event::kNoteOffEvent:
        midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0x80 | (e.noteOff.channel & 0x0F)), static_cast<uint8_t>(e.noteOff.pitch & 0x7F),
                      vel(e.noteOff.velocity));
        break;
      case sv::Event::kPolyPressureEvent:
        midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0xA0 | (e.polyPressure.channel & 0x0F)), static_cast<uint8_t>(e.polyPressure.pitch & 0x7F),
                      vel(e.polyPressure.pressure));
        break;
      case sv::Event::kLegacyMIDICCOutEvent: {
        const auto& m = e.midiCCOut;
        const uint8_t ch = static_cast<uint8_t>(m.channel & 0x0F);
        const uint8_t num = m.controlNumber;
        if (num < 128)
          midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0xB0 | ch), num, static_cast<uint8_t>(m.value & 0x7F));
        else if (num == sv::kAfterTouch)
          midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0xD0 | ch), static_cast<uint8_t>(m.value & 0x7F), 0);
        else if (num == sv::kPitchBend)
          midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(m.value & 0x7F), static_cast<uint8_t>(m.value2 & 0x7F));
        else if (num == sv::kCtrlProgramChange)
          midiOut_.push(e.sampleOffset, static_cast<uint8_t>(0xC0 | ch), static_cast<uint8_t>(m.value & 0x7F), 0);
        break;
      }
      default:
        break;
    }
  }
  // Processor-side parameter changes: last value of each queue for the UI.
  for (sb::int32 q = 0; q < outParams_.getParameterCount(); ++q) {
    sv::IParamValueQueue* vq = outParams_.getParameterData(q);
    sb::int32 np = vq ? vq->getPointCount() : 0;
    if (np <= 0) continue;
    sb::int32 off = 0;
    sv::ParamValue v = 0;
    vq->getPoint(np - 1, off, v);
    size_t h = outHead_.load(std::memory_order_relaxed);
    if (h - outTail_.load(std::memory_order_acquire) < kOutRing) {
      outRing_[h % kOutRing] = {vq->getParameterId(), v};
      outHead_.store(h + 1, std::memory_order_release);
    }
  }
  inParams_.clear();
  inEvents_.clear();
  busy_.store(false);
  return true;
}

}  // namespace gl::vst3

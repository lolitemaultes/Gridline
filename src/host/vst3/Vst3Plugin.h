// Gridline — VST3 hosting: one plugin instance (component + processor +
// edit controller), its buses, buffers and host callbacks.
//
// Threads: create / prepare / state / parameter info / editor on the UI thread;
// process() on the audio thread only. process() never allocates and never
// locks; reconfiguration suspends processing with an atomic handshake.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../Backend.h"
#include "Vst3HostObjects.h"
#include "Vst3Module.h"

namespace gl::vst3 {

struct ParamInfo {
  uint32_t id = 0;
  std::string title, units;
  int stepCount = 0;   // 0 continuous, 1 toggle, n: n+1 discrete values
  double def = 0;      // normalized default
  int32_t flags = 0;   // sv::ParameterInfo::ParameterFlags
  bool automatable() const { return (flags & sv::ParameterInfo::kCanAutomate) != 0; }
  bool exposed() const { return !(flags & (sv::ParameterInfo::kIsReadOnly | sv::ParameterInfo::kIsHidden | sv::ParameterInfo::kIsBypass)); }
};

// Calls from the plugin's controller, queued for the UI thread.
struct HostEvent {
  enum Kind { BeginEdit, PerformEdit, EndEdit, Restart, Dirty } kind = PerformEdit;
  uint32_t id = 0;
  double value = 0;
  int32_t flags = 0;  // Restart: sv::RestartFlags
};

// Notified when the instance is destroyed while an editor shows it.
class EditorHost {
 public:
  virtual ~EditorHost() = default;
  virtual void pluginGoingAway() = 0;  // UI thread; must close the view now
};

class Plugin {
 public:
  static constexpr int kMaxChannelsPerBus = 32;

  // Instantiates class `uid` of the module (UI thread).
  static std::unique_ptr<Plugin> create(std::shared_ptr<Module> module, const std::string& uid, std::string* err);
  ~Plugin();
  Plugin(const Plugin&) = delete;
  Plugin& operator=(const Plugin&) = delete;

  // ---- description ----
  const ClassInfo& classInfo() const { return info_; }
  const std::shared_ptr<Module>& module() const { return module_; }
  bool hasEventInput() const { return eventInputs_ > 0; }
  bool hasSidechain() const { return scBus_ >= 0; }
  int mainInputChannels() const { return mainIn_ >= 0 ? inCh_[static_cast<size_t>(mainIn_)] : 0; }
  int mainOutputChannels() const { return mainOut_ >= 0 ? outCh_[static_cast<size_t>(mainOut_)] : 0; }
  int sidechainChannels() const { return scBus_ >= 0 ? inCh_[static_cast<size_t>(scBus_)] : 0; }
  const std::vector<ParamInfo>& params() const { return params_; }
  int paramIndex(uint32_t id) const;  // index into params(), -1 when unknown
  bool supportsTuningExpression() const { return tuningExpr_; }
  // Note expression types the controller lists (sv::NoteExpressionTypeIDs).
  bool supportsExpression(uint32_t type) const {
    for (uint32_t t : exprTypes_)
      if (t == type) return true;
    return false;
  }
  bool hasEventOutput() const { return eventOutputs_ > 0; }
  int64_t bypassParam() const { return bypassId_; }  // -1 when none
  // MIDI controller → parameter (IMidiMapping): ctrl 0..127 CC, 128 channel
  // pressure, 129 pitch bend. -1 when the plugin maps none.
  int64_t midiParam(int channel, int ctrl) const {
    return midiMap_.empty() || channel < 0 || channel > 15 || ctrl < 0 || ctrl >= kMidiCtrls ? -1 : midiMap_[static_cast<size_t>(channel * kMidiCtrls + ctrl)];
  }
  // Program list of the root unit (IUnitInfo) and its program-change parameter (-1: none).
  const std::vector<std::string>& programs() const { return programs_; }
  int64_t programParam() const { return programParam_; }
  // Output buses beyond the main one (multi-output instruments), active after prepare().
  const std::vector<int>& auxOutputBuses() const { return auxOut_; }
  int outputBusChannels(int bus) const { return bus >= 0 && static_cast<size_t>(bus) < outCh_.size() ? outCh_[static_cast<size_t>(bus)] : 0; }
  std::string outputBusName(int bus) const;

  // ---- UI thread ----
  // Prepares (or re-prepares) processing: bus layout, buffers, activation.
  bool prepare(double sampleRate, int maxBlock);
  // Offline processing mode (export): set up again with kOffline.
  void setOffline(bool offline);
  bool offline() const { return offline_; }
  double sampleRate() const { return sampleRate_; }
  int maxBlock() const { return maxBlock_; }
  bool active() const { return active_; }
  // Re-reads the bus layout and latency after the plugin asked for it.
  void restart(int32_t flags);
  int latency() const { return latency_.load(std::memory_order_relaxed); }
  int tailSamples() const { return tail_; }
  bool getState(std::string& component, std::string& controller);
  bool setState(const std::string& component, const std::string& controller);
  // Controller side of parameters.
  double paramNormalized(uint32_t id) const;
  void setParamNormalized(uint32_t id, double value);
  std::string paramText(uint32_t id, double normalized) const;
  sv::IEditController* controller() const { return controller_.get(); }
  // Pending controller calls (performEdit, restartComponent…).
  void drainHostEvents(std::vector<HostEvent>& out);
  // Parameter values the processor reported (outputParameterChanges).
  struct OutputValue {
    uint32_t id;
    double value;
  };
  bool popOutputValue(OutputValue& v);
  void setEditorHost(EditorHost* h) { editorHost_ = h; }
  EditorHost* editorHost() const { return editorHost_; }

  // ---- audio thread ----
  // Input buffers to fill before process(): main input / sidechain channels.
  float* inputChannel(int bus, int ch) { return inPtr_[static_cast<size_t>(bus)][static_cast<size_t>(ch)]; }
  float* outputChannel(int bus, int ch) { return outPtr_[static_cast<size_t>(bus)][static_cast<size_t>(ch)]; }
  int mainInputBus() const { return mainIn_; }
  int mainOutputBus() const { return mainOut_; }
  int sidechainBus() const { return scBus_; }
  ParameterChanges& inputParams() { return inParams_; }
  EventList& inputEvents() { return inEvents_; }
  // Runs one block of n <= maxBlock() frames with the queued parameter
  // changes and events (both cleared afterwards). False when not active
  // (outputs are then silent).
  bool process(int n, const sv::ProcessContext& ctx);
  // Note / MIDI output of the last process() (event output bus).
  host::MidiOutBuffer& midiOut() { return midiOut_; }

  // Called by the component handler (any thread).
  void pushHostEvent(const HostEvent& e);

 private:
  Plugin() = default;
  bool setupBuses();
  void deactivate();
  void readParams();
  void readMidiMapping();
  void readPrograms();

  std::shared_ptr<Module> module_;
  ClassInfo info_;
  Ptr<sv::IComponent> component_;
  Ptr<sv::IAudioProcessor> processor_;
  Ptr<sv::IEditController> controller_;
  bool singleComponent_ = false;  // component implements the controller too
  Ptr<sv::IConnectionPoint> compCP_, ctrlCP_;
  struct Handler;
  std::unique_ptr<Handler> handler_;

  std::vector<ParamInfo> params_;
  std::vector<std::pair<uint32_t, int>> paramLookup_;  // (id, index) sorted by id
  int64_t bypassId_ = -1;
  bool tuningExpr_ = false;
  std::vector<uint32_t> exprTypes_;
  int eventOutputs_ = 0;
  bool offline_ = false;
  host::MidiOutBuffer midiOut_;
  static constexpr int kMidiCtrls = 130;
  std::vector<int64_t> midiMap_;  // 16 channels x kMidiCtrls
  std::vector<std::string> programs_;
  int64_t programParam_ = -1;
  std::vector<int> auxOut_;

  // buses
  int eventInputs_ = 0;
  int mainIn_ = -1, mainOut_ = -1, scBus_ = -1;
  std::vector<int> inCh_, outCh_;
  std::vector<sv::AudioBusBuffers> inBus_, outBus_;
  std::vector<std::vector<float*>> inPtr_, outPtr_;
  std::vector<float> storage_;

  double sampleRate_ = 0;
  int maxBlock_ = 0;
  bool active_ = false;
  bool processingOn_ = false;
  std::atomic<int> latency_{0};
  int tail_ = 0;

  // process() handshake
  std::atomic<bool> suspended_{true};
  std::atomic<bool> busy_{false};

  ParameterChanges inParams_, outParams_;
  EventList inEvents_, outEvents_;

  host::HostEventQueue hostQueue_;  // any thread → UI, lock-free (plugins call the handler from the audio thread too)
  // Single-producer (audio) / single-consumer (UI) ring of output values.
  static constexpr size_t kOutRing = 1024;
  OutputValue outRing_[kOutRing] = {};
  std::atomic<size_t> outHead_{0}, outTail_{0};

  EditorHost* editorHost_ = nullptr;
};

}  // namespace gl::vst3

namespace gl::host {
class Backend;
}
namespace gl::vst3 {
// The VST3 plugin behind a host::Backend (nullptr for other formats).
Plugin* pluginOf(host::Backend* b);
}  // namespace gl::vst3

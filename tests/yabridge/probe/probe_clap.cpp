// Gridline yabridge test: the probe as a Windows CLAP (one instrument and one effect), written
// against the CLAP headers only. See probe_core.h.
#include "probe_core.h"
#include <clap/clap.h>
#include <new>

namespace {

const char* const kSynthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
const char* const kFxFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kSynthDesc = {CLAP_VERSION_INIT, "org.gridline.yabridge-probe.synth", "Gridline Probe Synth", "Gridline Tests", "", "", "",
                                             "1.0.0", "yabridge test probe", kSynthFeatures};
const clap_plugin_descriptor_t kFxDesc = {CLAP_VERSION_INIT, "org.gridline.yabridge-probe.fx", "Gridline Probe Fx", "Gridline Tests", "", "", "",
                                          "1.0.0", "yabridge test probe", kFxFeatures};

// Edits made in the window, waiting for the next flush or process call.
struct Edit {
  int kind;  // 0 begin, 1 value, 2 end
  int param;
  double value;
};

struct Plugin final : probe::Host {
  clap_plugin_t plugin{};
  const clap_host_t* host;
  probe::Core core;
  probe::Window window;
  int w = probe::kDefaultW, h = probe::kDefaultH;
  static constexpr uint32_t kQueue = 256;
  Edit queue[kQueue] = {};
  std::atomic<uint32_t> head{0}, tail{0};

  Plugin(const clap_host_t* hostIn, bool synth) : host(hostIn), core(synth), window(core, *this) {}
  void push(const Edit& e) {
    const uint32_t t = tail.load();
    if (t - head.load() >= kQueue) return;
    queue[t % kQueue] = e;
    tail.store(t + 1);
    if (auto* hp = static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS))) hp->request_flush(host);
  }
  void beginEdit(int param) override { push({0, param, 0}); }
  void performEdit(int param, double value) override { push({1, param, value}); }
  void endEdit(int param) override { push({2, param, 0}); }
  bool requestSize(int nw, int nh) override {
    auto* hg = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI));
    return hg && hg->request_resize(host, static_cast<uint32_t>(nw), static_cast<uint32_t>(nh));
  }
  void inputEvents(const clap_input_events_t* in) {
    if (!in) return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
      const clap_event_header_t* e = in->get(in, i);
      if (!e || e->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
      if (e->type == CLAP_EVENT_PARAM_VALUE) {
        auto* v = reinterpret_cast<const clap_event_param_value_t*>(e);
        if (v->param_id < probe::kParams) core.p[v->param_id].store(v->value);
      } else if (e->type == CLAP_EVENT_NOTE_ON) {
        auto* v = reinterpret_cast<const clap_event_note_t*>(e);
        core.noteOn(v->key, v->velocity);
      } else if (e->type == CLAP_EVENT_NOTE_OFF) {
        core.noteOff(reinterpret_cast<const clap_event_note_t*>(e)->key);
      }
    }
  }
  void outputEvents(const clap_output_events_t* out) {
    if (!out) return;
    uint32_t r = head.load();
    const uint32_t t = tail.load();
    for (; r != t; ++r) {
      const Edit& e = queue[r % kQueue];
      if (e.kind == 1) {
        clap_event_param_value_t v{};
        v.header = {sizeof v, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = static_cast<clap_id>(e.param);
        v.note_id = v.port_index = v.channel = v.key = -1;
        v.value = e.value;
        out->try_push(out, &v.header);
      } else {
        clap_event_param_gesture_t g{};
        g.header = {sizeof g, 0, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(e.kind == 0 ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END), 0};
        g.param_id = static_cast<clap_id>(e.param);
        out->try_push(out, &g.header);
      }
    }
    head.store(r);
  }
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// ---- audio ports, note ports
uint32_t audioCount(const clap_plugin_t* p, bool isInput) { return isInput && self(p)->core.synth ? 0 : 1; }
bool audioGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
  if (index >= audioCount(p, isInput)) return false;
  *info = clap_audio_port_info_t{};
  info->id = isInput ? 1 : 2;
  std::strcpy(info->name, "Main");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kAudioPorts = {audioCount, audioGet};
uint32_t noteCount(const clap_plugin_t* p, bool isInput) { return isInput && self(p)->core.synth ? 1 : 0; }
bool noteGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_note_port_info_t* info) {
  if (index >= noteCount(p, isInput)) return false;
  *info = clap_note_port_info_t{};
  info->id = 3;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::strcpy(info->name, "Notes");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {noteCount, noteGet};

// ---- parameters
uint32_t paramCount(const clap_plugin_t*) { return probe::kParams; }
bool paramInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index >= probe::kParams) return false;
  *info = clap_param_info_t{};
  info->id = index;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE | (index == 2 ? CLAP_PARAM_IS_STEPPED : 0u);
  std::strcpy(info->name, probe::kParamNames[index]);
  info->min_value = 0;
  info->max_value = 1;
  info->default_value = probe::kParamDefaults[index];
  return true;
}
bool paramValue(const clap_plugin_t* p, clap_id id, double* v) {
  if (id >= probe::kParams) return false;
  *v = self(p)->core.p[id].load();
  return true;
}
bool paramText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
  std::snprintf(out, size, "%.3f", v);
  return true;
}
bool paramParse(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
  self(p)->inputEvents(in);
  self(p)->outputEvents(out);
}
const clap_plugin_params_t kParams = {paramCount, paramInfo, paramValue, paramText, paramParse, paramFlush};

// ---- state
bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
  char buf[probe::Core::kStateSize];
  self(p)->core.saveState(buf);
  return s->write(s, buf, sizeof buf) == static_cast<int64_t>(sizeof buf);
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
  char buf[probe::Core::kStateSize];
  const int64_t got = s->read(s, buf, sizeof buf);
  return got > 0 && self(p)->core.loadState(buf, static_cast<size_t>(got));
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---- latency
uint32_t latencyGet(const clap_plugin_t* p) { return self(p)->core.synth ? 0 : probe::kLatency; }
const clap_plugin_latency_t kLatencyExt = {latencyGet};

// ---- interface
bool guiApi(const clap_plugin_t*, const char* api, bool floating) { return !floating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0; }
bool guiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
  *api = CLAP_WINDOW_API_WIN32;
  *floating = false;
  return true;
}
bool guiCreate(const clap_plugin_t*, const char* api, bool floating) { return !floating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0; }
void guiDestroy(const clap_plugin_t* p) { self(p)->window.close(); }
bool guiScale(const clap_plugin_t* p, double scale) {
  probe::logf(self(p)->core.inst, "scale %.3f", scale);
  return true;
}
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
  *w = static_cast<uint32_t>(self(p)->w);
  *h = static_cast<uint32_t>(self(p)->h);
  return true;
}
bool guiCanResize(const clap_plugin_t*) { return true; }
bool guiHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  *hints = clap_gui_resize_hints_t{};
  hints->can_resize_horizontally = hints->can_resize_vertically = true;
  return true;
}
bool guiAdjust(const clap_plugin_t*, uint32_t*, uint32_t*) { return true; }
bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
  self(p)->w = static_cast<int>(w);
  self(p)->h = static_cast<int>(h);
  self(p)->window.resize(static_cast<int>(w), static_cast<int>(h));
  return true;
}
bool guiParent(const clap_plugin_t* p, const clap_window_t* win) {
  return self(p)->window.open(static_cast<HWND>(win->win32), self(p)->w, self(p)->h);
}
bool guiTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui = {guiApi, guiPreferred, guiCreate, guiDestroy, guiScale, guiGetSize, guiCanResize, guiHints,
                                guiAdjust, guiSetSize, guiParent, guiTransient, guiTitle, guiShow, guiHide};

// ---- the plugin
bool pluginInit(const clap_plugin_t*) { return true; }
void pluginDestroy(const clap_plugin_t* p) {
  probe::logf(self(p)->core.inst, "destroy");
  delete self(p);
}
bool pluginActivate(const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
  self(p)->core.sr = rate;
  probe::logf(self(p)->core.inst, "setup rate %.0f block %u mode 0", rate, maxFrames);
  probe::logf(self(p)->core.inst, "active 1");
  return true;
}
void pluginDeactivate(const clap_plugin_t* p) {
  probe::logf(self(p)->core.inst, "active 0");
  self(p)->core.logAudio();
}
bool pluginStart(const clap_plugin_t*) { return true; }
void pluginStop(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}
clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* d) {
  Plugin* s = self(p);
  s->inputEvents(d->in_events);
  s->outputEvents(d->out_events);
  if (d->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
  const float* const* in = !s->core.synth && d->audio_inputs_count > 0 ? const_cast<const float* const*>(d->audio_inputs[0].data32) : nullptr;
  s->core.render(in, d->audio_outputs[0].data32, static_cast<int32_t>(d->frames_count));
  return CLAP_PROCESS_CONTINUE;
}
const void* pluginExt(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatencyExt;
  if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
  return nullptr;
}
void pluginMain(const clap_plugin_t*) {}

// ---- factory and entry
uint32_t factoryCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kSynthDesc : i == 1 ? &kFxDesc : nullptr; }
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  const bool synth = !std::strcmp(id, kSynthDesc.id);
  if (!synth && std::strcmp(id, kFxDesc.id) != 0) return nullptr;
  auto* s = new (std::nothrow) Plugin(host, synth);
  if (!s) return nullptr;
  s->plugin.desc = synth ? &kSynthDesc : &kFxDesc;
  s->plugin.plugin_data = s;
  s->plugin.init = pluginInit;
  s->plugin.destroy = pluginDestroy;
  s->plugin.activate = pluginActivate;
  s->plugin.deactivate = pluginDeactivate;
  s->plugin.start_processing = pluginStart;
  s->plugin.stop_processing = pluginStop;
  s->plugin.reset = pluginReset;
  s->plugin.process = pluginProcess;
  s->plugin.get_extension = pluginExt;
  s->plugin.on_main_thread = pluginMain;
  return &s->plugin;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDesc, factoryCreate};
bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" __declspec(dllexport) const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};

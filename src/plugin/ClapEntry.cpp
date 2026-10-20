// Gridline — CLAP entry point of Gridline as a plugin (PH-09), written
// against the CLAP interface headers (MIT). Two plugins:
//   org.gridline.gridline      "Gridline"     instrument: notes in, stereo out
//                                              + 15 stereo outputs (inserts 1-15)
//   org.gridline.gridline-fx   "Gridline FX"  audio effect: stereo in (mixer
//                                              insert 1) + sidechain, stereo out
// Extensions: audio ports, note ports, params (16 macros), state (the whole
// project), latency, render (offline), GUI (Cocoa / X11, resizable) and the
// host's timer support (X11 editor redraws, idle work).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "clap/clap.h"
#include "PluginCore.h"
#include "PluginView.h"

using namespace gl::plugin;

namespace {

const char* const kFeaturesInstrument[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_SAMPLER,
                                           CLAP_PLUGIN_FEATURE_DRUM_MACHINE, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kFeaturesEffect[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_MIXING, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDescInstrument = {
    CLAP_VERSION_INIT, "org.gridline.gridline", "Gridline", "Gridline", "https://github.com/lolitemaultes/Gridline", "", "",
    GRIDLINE_VERSION_STRING, "The Gridline workstation as an instrument: patterns, playlist, mixer and instruments.", kFeaturesInstrument};
const clap_plugin_descriptor_t kDescEffect = {
    CLAP_VERSION_INIT, "org.gridline.gridline-fx", "Gridline FX", "Gridline", "https://github.com/lolitemaultes/Gridline", "", "",
    GRIDLINE_VERSION_STRING, "The Gridline workstation as an effect: the input runs through mixer insert 1.", kFeaturesEffect};

constexpr int kMaxEvents = 4096;
constexpr uint32_t kIdleTimerMs = 30;

struct Plugin {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  const clap_host_latency_t* hostLatency = nullptr;
  const clap_host_params_t* hostParams = nullptr;
  const clap_host_timer_support_t* hostTimer = nullptr;
  const clap_host_gui_t* hostGui = nullptr;
  Variant variant;
  std::unique_ptr<Instance> inst;
  std::unique_ptr<View> view;
  clap_id timer = CLAP_INVALID_ID;
  bool activeNow = false;
  std::atomic<uint32_t> edited{0};    // macros changed on the main thread: told to the host from process / flush
  std::atomic<bool> latencyDirty{false};
  int64_t samplesSinceCallback = 0;    // audio thread: idle requests without a host timer
  int idleEvery = 2048;
  Event events[kMaxEvents];
  explicit Plugin(Variant v) : variant(v) {}
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// ---------------------------------------------------------------- events

// Host input events → Instance events (notes become MIDI bytes, parameter
// values macro changes). Returns the count.
int translate(Plugin& pl, const clap_input_events_t* in) {
  int n = 0;
  if (!in) return 0;
  const uint32_t count = in->size(in);
  for (uint32_t i = 0; i < count && n < kMaxEvents; ++i) {
    const clap_event_header_t* h = in->get(in, i);
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
    Event e;
    e.offset = static_cast<int>(h->time);
    switch (h->type) {
      case CLAP_EVENT_NOTE_ON:
      case CLAP_EVENT_NOTE_OFF: {
        const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
        if (ne->key < 0 || ne->key > 127) continue;
        const int ch = ne->channel >= 0 && ne->channel < 16 ? ne->channel : 0;
        const int vel = static_cast<int>(std::lround(std::clamp(ne->velocity, 0.0, 1.0) * 127.0));
        e.kind = Event::Midi;
        if (h->type == CLAP_EVENT_NOTE_ON) {
          e.midi[0] = static_cast<uint8_t>(0x90 | ch);
          e.midi[2] = static_cast<uint8_t>(std::max(1, vel));
        } else {
          e.midi[0] = static_cast<uint8_t>(0x80 | ch);
          e.midi[2] = static_cast<uint8_t>(vel);
        }
        e.midi[1] = static_cast<uint8_t>(ne->key);
        break;
      }
      case CLAP_EVENT_NOTE_CHOKE: {
        const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
        if (ne->key < 0 || ne->key > 127) continue;
        e.kind = Event::Midi;
        e.midi[0] = static_cast<uint8_t>(0x80 | (ne->channel >= 0 && ne->channel < 16 ? ne->channel : 0));
        e.midi[1] = static_cast<uint8_t>(ne->key);
        break;
      }
      case CLAP_EVENT_MIDI: {
        const auto* me = reinterpret_cast<const clap_event_midi_t*>(h);
        e.kind = Event::Midi;
        std::memcpy(e.midi, me->data, 3);
        break;
      }
      case CLAP_EVENT_PARAM_VALUE: {
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
        if (pv->param_id >= static_cast<clap_id>(kMacros)) continue;
        e.kind = Event::Macro;
        e.macro = static_cast<int>(pv->param_id);
        e.value = static_cast<float>(pv->value);
        break;
      }
      default: continue;
    }
    pl.events[n++] = e;
  }
  return n;
}

// Macros changed on the main thread → parameter value events for the host.
void pushEdits(Plugin& pl, const clap_output_events_t* out) {
  uint32_t bits = pl.edited.exchange(0);
  if (!bits || !out) return;
  for (int k = 0; k < kMacros; ++k) {
    if (!(bits & (1u << k))) continue;
    clap_event_param_value_t ev{};
    ev.header.size = sizeof ev;
    ev.header.time = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags = 0;
    ev.param_id = static_cast<clap_id>(k);
    ev.note_id = -1;
    ev.port_index = ev.channel = ev.key = -1;
    ev.value = pl.inst->macroValue(k);
    out->try_push(out, &ev.header);
  }
}

// ---------------------------------------------------------------- audio ports

uint32_t portsCount(const clap_plugin_t* p, bool isInput) {
  const Plugin& pl = *self(p);
  if (pl.variant == Variant::Effect) return isInput ? 2 : 1;
  return isInput ? 0 : static_cast<uint32_t>(1 + kExtraOutputs);
}

bool portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
  if (index >= portsCount(p, isInput)) return false;
  const Plugin& pl = *self(p);
  *info = clap_audio_port_info_t{};
  info->id = index;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  if (index == 0) info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  if (pl.variant == Variant::Effect)
    std::snprintf(info->name, sizeof info->name, "%s", index == 0 ? "Main" : "Sidechain");
  else if (index == 0)
    std::snprintf(info->name, sizeof info->name, "Main");
  else
    std::snprintf(info->name, sizeof info->name, "Insert %u", index);
  return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {portsCount, portsGet};

uint32_t notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
  if (!isInput || index != 0) return false;
  *info = clap_note_port_info_t{};
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
  std::snprintf(info->name, sizeof info->name, "Notes");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// ---------------------------------------------------------------- params

uint32_t paramsCount(const clap_plugin_t*) { return static_cast<uint32_t>(kMacros); }
bool paramsInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
  if (index >= static_cast<uint32_t>(kMacros)) return false;
  *info = clap_param_info_t{};
  info->id = index;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  info->cookie = nullptr;
  std::snprintf(info->name, sizeof info->name, "%s", self(p)->inst->macroName(static_cast<int>(index)).c_str());
  std::snprintf(info->module, sizeof info->module, "Macros");
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = 0.0;
  return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* out) {
  if (id >= static_cast<clap_id>(kMacros)) return false;
  *out = self(p)->inst->macroValue(static_cast<int>(id));
  return true;
}
bool paramsToText(const clap_plugin_t* p, clap_id id, double value, char* out, uint32_t size) {
  if (id >= static_cast<clap_id>(kMacros) || !out || size == 0) return false;
  std::snprintf(out, size, "%s", self(p)->inst->macroText(static_cast<int>(id), value).c_str());
  return true;
}
bool paramsFromText(const clap_plugin_t*, clap_id id, const char* text, double* out) {
  if (id >= static_cast<clap_id>(kMacros) || !text) return false;
  char* end = nullptr;
  double v = std::strtod(text, &end);
  if (end == text) return false;
  if (end && *end == '%') v /= 100.0;
  *out = std::clamp(v, 0.0, 1.0);
  return true;
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
  Plugin& pl = *self(p);
  const int n = translate(pl, in);
  for (int i = 0; i < n; ++i)
    if (pl.events[i].kind == Event::Macro) pl.inst->setMacroValue(pl.events[i].macro, pl.events[i].value);
  pushEdits(pl, out);
}
const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

// ---------------------------------------------------------------- state

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
  const std::string data = self(p)->inst->saveState();
  size_t done = 0;
  while (done < data.size()) {
    const int64_t w = s->write(s, data.data() + done, data.size() - done);
    if (w <= 0) return false;
    done += static_cast<size_t>(w);
  }
  return true;
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
  std::string data;
  char buf[65536];
  for (;;) {
    const int64_t r = s->read(s, buf, sizeof buf);
    if (r < 0) return false;
    if (r == 0) break;
    data.append(buf, static_cast<size_t>(r));
  }
  Plugin& pl = *self(p);
  const bool ok = pl.inst->loadState(data);
  if (ok && pl.hostParams) pl.hostParams->rescan(pl.host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
  return ok;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---------------------------------------------------------------- latency, render

uint32_t latencyGet(const clap_plugin_t* p) { return static_cast<uint32_t>(std::max(0, self(p)->inst->latency())); }
const clap_plugin_latency_t kLatency = {latencyGet};

bool renderRealtime(const clap_plugin_t*) { return false; }
bool renderSet(const clap_plugin_t* p, clap_plugin_render_mode mode) {
  self(p)->inst->setOffline(mode == CLAP_RENDER_OFFLINE);
  return true;
}
const clap_plugin_render_t kRender = {renderRealtime, renderSet};

// ---------------------------------------------------------------- timer

void onTimer(const clap_plugin_t* p, clap_id) {
  Plugin& pl = *self(p);
  if (pl.view) pl.view->tick();
  pl.inst->idle();
}
const clap_plugin_timer_support_t kTimer = {onTimer};

// ---------------------------------------------------------------- GUI

const char* apiName(WindowApi a) { return a == WindowApi::Cocoa ? CLAP_WINDOW_API_COCOA : CLAP_WINDOW_API_X11; }
bool guiApiSupported(const clap_plugin_t*, const char* api, bool floating) {
  return !floating && api && std::strcmp(api, apiName(nativeWindowApi())) == 0 && viewSupported(nativeWindowApi());
}
bool guiPreferredApi(const clap_plugin_t*, const char** api, bool* floating) {
  if (!viewSupported(nativeWindowApi())) return false;
  *api = apiName(nativeWindowApi());
  *floating = false;
  return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) {
  Plugin& pl = *self(p);
  if (!guiApiSupported(p, api, floating) || pl.view) return false;
  pl.view = createView(*pl.inst);
  if (!pl.view) return false;
  pl.view->requestResize = [&pl](int w, int h) {
    return pl.hostGui && pl.hostGui->request_resize(pl.host, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  };
  return true;
}
void guiDestroy(const clap_plugin_t* p) {
  Plugin& pl = *self(p);
  if (pl.view) pl.view->detach();
  pl.view.reset();
}
bool guiSetScale(const clap_plugin_t* p, double scale) {
  Plugin& pl = *self(p);
  if (!pl.view || nativeWindowApi() == WindowApi::Cocoa) return false;  // macOS: backing scale
  pl.view->setScale(scale);
  return true;
}
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
  Plugin& pl = *self(p);
  if (!pl.view) return false;
  int vw = 0, vh = 0;
  pl.view->size(vw, vh);
  *w = static_cast<uint32_t>(vw);
  *h = static_cast<uint32_t>(vh);
  return true;
}
bool guiCanResize(const clap_plugin_t*) { return true; }
bool guiResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  hints->can_resize_horizontally = true;
  hints->can_resize_vertically = true;
  hints->preserve_aspect_ratio = false;
  hints->aspect_ratio_width = hints->aspect_ratio_height = 0;
  return true;
}
bool guiAdjustSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
  *w = std::max<uint32_t>(*w, kMinViewW);
  *h = std::max<uint32_t>(*h, kMinViewH);
  return true;
}
bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
  Plugin& pl = *self(p);
  if (!pl.view) return false;
  pl.view->setSize(static_cast<int>(std::max<uint32_t>(w, kMinViewW)), static_cast<int>(std::max<uint32_t>(h, kMinViewH)));
  return true;
}
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
  Plugin& pl = *self(p);
  if (!pl.view || !window) return false;
  void* parent = nativeWindowApi() == WindowApi::Cocoa ? window->cocoa : reinterpret_cast<void*>(static_cast<uintptr_t>(window->x11));
  return pl.view->attach(parent);
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t* p) {
  Plugin& pl = *self(p);
  if (!pl.view) return false;
  pl.view->setVisible(true);
  return true;
}
bool guiHide(const clap_plugin_t* p) {
  Plugin& pl = *self(p);
  if (!pl.view) return false;
  pl.view->setVisible(false);
  return true;
}
const clap_plugin_gui_t kGui = {guiApiSupported, guiPreferredApi, guiCreate,      guiDestroy,      guiSetScale,
                                guiGetSize,      guiCanResize,    guiResizeHints, guiAdjustSize,   guiSetSize,
                                guiSetParent,    guiSetTransient, guiSuggestTitle, guiShow,        guiHide};

// ---------------------------------------------------------------- plugin

bool pluginInit(const clap_plugin_t* p) {
  Plugin& pl = *self(p);
  const clap_host_t* h = pl.host;
  pl.hostLatency = static_cast<const clap_host_latency_t*>(h->get_extension(h, CLAP_EXT_LATENCY));
  pl.hostParams = static_cast<const clap_host_params_t*>(h->get_extension(h, CLAP_EXT_PARAMS));
  pl.hostTimer = static_cast<const clap_host_timer_support_t*>(h->get_extension(h, CLAP_EXT_TIMER_SUPPORT));
  pl.hostGui = static_cast<const clap_host_gui_t*>(h->get_extension(h, CLAP_EXT_GUI));
  pl.inst = std::make_unique<Instance>(pl.variant);
  pl.inst->onLatencyChanged = [&pl]() {
    // The host takes a new latency while the plugin is deactivated.
    if (!pl.activeNow && pl.hostLatency)
      pl.hostLatency->changed(pl.host);
    else
      pl.host->request_restart(pl.host);
  };
  pl.inst->onMacrosChanged = [&pl]() {
    if (pl.hostParams) pl.hostParams->rescan(pl.host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
  };
  pl.inst->onMacroEdited = [&pl](int k, double) {
    pl.edited.fetch_or(1u << k);
    if (pl.hostParams) pl.hostParams->request_flush(pl.host);
  };
  if (pl.hostTimer && pl.hostTimer->register_timer) pl.hostTimer->register_timer(h, kIdleTimerMs, &pl.timer);
  return true;
}

void pluginDestroy(const clap_plugin_t* p) {
  Plugin* pl = self(p);
  if (pl->hostTimer && pl->timer != CLAP_INVALID_ID) pl->hostTimer->unregister_timer(pl->host, pl->timer);
  if (pl->view) pl->view->detach();
  pl->view.reset();
  pl->inst.reset();
  delete pl;
}

bool pluginActivate(const clap_plugin_t* p, double sr, uint32_t, uint32_t maxFrames) {
  Plugin& pl = *self(p);
  if (!pl.inst->activate(sr, static_cast<int>(maxFrames))) return false;
  pl.activeNow = true;
  pl.idleEvery = std::max(256, static_cast<int>(sr * kIdleTimerMs / 1000.0));
  return true;
}
void pluginDeactivate(const clap_plugin_t* p) {
  Plugin& pl = *self(p);
  pl.inst->deactivate();
  pl.activeNow = false;
}
bool pluginStart(const clap_plugin_t*) { return true; }
void pluginStop(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}

clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* pr) {
  Plugin& pl = *self(p);
  const int frames = static_cast<int>(pr->frames_count);
  if (frames <= 0) return CLAP_PROCESS_CONTINUE;
  if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 1 || !pr->audio_outputs[0].data32) return CLAP_PROCESS_ERROR;
  ProcessArgs a;
  a.frames = frames;
  const clap_audio_buffer_t& out = pr->audio_outputs[0];
  a.outL = out.data32[0];
  a.outR = out.channel_count > 1 ? out.data32[1] : out.data32[0];
  float* xl[kExtraOutputs];
  float* xr[kExtraOutputs];
  if (pl.variant == Variant::Instrument) {
    int extra = 0;
    for (uint32_t b = 1; b < pr->audio_outputs_count && extra < kExtraOutputs; ++b) {
      const clap_audio_buffer_t& ob = pr->audio_outputs[b];
      if (ob.channel_count < 1 || !ob.data32) break;
      xl[extra] = ob.data32[0];
      xr[extra] = ob.channel_count > 1 ? ob.data32[1] : ob.data32[0];
      ++extra;
    }
    a.extraCount = extra;
    a.extraL = xl;
    a.extraR = xr;
  } else {
    if (pr->audio_inputs_count > 0 && pr->audio_inputs[0].channel_count > 0 && pr->audio_inputs[0].data32) {
      a.inL = pr->audio_inputs[0].data32[0];
      a.inR = pr->audio_inputs[0].channel_count > 1 ? pr->audio_inputs[0].data32[1] : a.inL;
    }
    if (pr->audio_inputs_count > 1 && pr->audio_inputs[1].channel_count > 0 && pr->audio_inputs[1].data32) {
      a.scL = pr->audio_inputs[1].data32[0];
      a.scR = pr->audio_inputs[1].channel_count > 1 ? pr->audio_inputs[1].data32[1] : a.scL;
    }
  }
  a.eventCount = translate(pl, pr->in_events);
  a.events = pl.events;
  if (const clap_event_transport_t* t = pr->transport) {
    a.transport.valid = true;
    a.transport.playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    a.transport.hasTempo = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
    a.transport.bpm = t->tempo;
    a.transport.hasPosition = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
    a.transport.beats = static_cast<double>(t->song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR);
  }
  pl.inst->process(a);
  pushEdits(pl, pr->out_events);
  // Without a host timer, the main-thread work is asked for every ~30 ms.
  if (!pl.hostTimer || pl.timer == CLAP_INVALID_ID) {
    pl.samplesSinceCallback += frames;
    if (pl.samplesSinceCallback >= pl.idleEvery) {
      pl.samplesSinceCallback = 0;
      pl.host->request_callback(pl.host);
    }
  }
  return CLAP_PROCESS_CONTINUE;
}

const void* pluginExtension(const clap_plugin_t* p, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
  if (!std::strcmp(id, CLAP_EXT_RENDER)) return &kRender;
  if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimer;
  if (!std::strcmp(id, CLAP_EXT_GUI) && viewSupported(nativeWindowApi())) return &kGui;
  (void)p;
  return nullptr;
}

void pluginOnMainThread(const clap_plugin_t* p) { self(p)->inst->idle(); }

const clap_plugin_t* createPlugin(const clap_host_t* host, Variant v) {
  auto* pl = new Plugin(v);
  pl->host = host;
  clap_plugin_t& c = pl->plugin;
  c.desc = v == Variant::Effect ? &kDescEffect : &kDescInstrument;
  c.plugin_data = pl;
  c.init = pluginInit;
  c.destroy = pluginDestroy;
  c.activate = pluginActivate;
  c.deactivate = pluginDeactivate;
  c.start_processing = pluginStart;
  c.stop_processing = pluginStop;
  c.reset = pluginReset;
  c.process = pluginProcess;
  c.get_extension = pluginExtension;
  c.on_main_thread = pluginOnMainThread;
  return &c;
}

// ---------------------------------------------------------------- factory

uint32_t factoryCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index == 0 ? &kDescInstrument : index == 1 ? &kDescEffect : nullptr;
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!host || !id || !clap_version_is_compatible(host->clap_version)) return nullptr;
  if (!std::strcmp(id, kDescInstrument.id)) return createPlugin(host, Variant::Instrument);
  if (!std::strcmp(id, kDescEffect.id)) return createPlugin(host, Variant::Effect);
  return nullptr;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
}

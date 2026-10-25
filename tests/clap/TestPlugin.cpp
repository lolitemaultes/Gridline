// Gridline — minimal CLAP plugin used by the host tests (built in-tree as
// GridlineTest.clap; never installed). Written against the CLAP headers only.
//   "org.gridline.test.synth"  instrument: one sine voice, CLAP + MIDI note
//       dialects, tuning note expression, MIDI pitch bend (+-2 semitones), a
//       second output port ("Sub", one octave lower); MIDI CC 20 makes it
//       report a Gain edit as if made in its own editor; key 1 crashes it on
//       purpose (sandbox test). Gain is modulatable (PARAM_MOD). "Echo" sends
//       every note it receives back out one octave higher (note output
//       port, like an arpeggiator). Two presets through preset discovery
//       (plugin location) + preset-load. Its own interface on macOS (a
//       fixed 320 x 200 Cocoa view, clap.gui).
//   "org.gridline.test.gain"   effect: level (applied sample-accurately),
//       sidechain input mixed in by a parameter, 16 samples latency or 64
//       with "Long" (asks the host for a restart), a bypass parameter.
// Both record what the host told them (transport, render mode) in a probe
// the tests read through the exported gridline_clap_probe().
#include <clap/clap.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Probe.h"
#if defined(__APPLE__)
#include "../common/TestView.h"
#endif

namespace {

GridlineClapProbe gProbe;

struct Synth {
  clap_plugin_t plugin;
  const clap_host_t* host;
  double sr = 48000;
  std::atomic<double> gain{0.5}, wave{0}, mode{0}, echo{0};
  double mod = 0;  // modulation on Gain (audio thread)
  std::atomic<int32_t> notes{0};
  int32_t noteId = -1;
  int key = 60;
  double tuning = 0, expr = 0, bend = 0, amp = 0, phase = 0, subPhase = 0;
  bool gui = false;     // clap.gui created
  void* view = nullptr;  // its view (macOS)
};

struct Gain {
  clap_plugin_t plugin;
  const clap_host_t* host;
  std::atomic<double> level{1.0}, side{0.0}, longMode{0}, bypass{0};
  std::atomic<int> latency{16};
  float delay[2][64] = {};
  int pos = 0;
  bool active = false;
};

enum : clap_id { kGain = 1, kWave = 2, kMode = 3, kEcho = 4, kLevel = 10, kSide = 11, kLong = 12, kBypass = 13 };

const char* const kSynthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kGainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

const clap_plugin_descriptor_t kSynthDesc = {CLAP_VERSION_INIT, "org.gridline.test.synth", "Gridline Test Synth (CLAP)", "Gridline Tests", "", "", "",
                                             "1.0.0", "test instrument", kSynthFeatures};
const clap_plugin_descriptor_t kGainDesc = {CLAP_VERSION_INIT, "org.gridline.test.gain", "Gridline Test Gain (CLAP)", "Gridline Tests", "", "", "",
                                            "1.0.0", "test effect", kGainFeatures};

Synth* synthOf(const clap_plugin_t* p) { return static_cast<Synth*>(p->plugin_data); }
Gain* gainOf(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

template <class T>
bool writeAll(const clap_ostream_t* s, const T& v) {
  return s->write(s, &v, sizeof v) == static_cast<int64_t>(sizeof v);
}
template <class T>
bool readAll(const clap_istream_t* s, T& v) {
  return s->read(s, &v, sizeof v) == static_cast<int64_t>(sizeof v);
}

void setName(char* dst, const char* src) {
  std::strncpy(dst, src, CLAP_NAME_SIZE - 1);
  dst[CLAP_NAME_SIZE - 1] = 0;
}

void noteTransport(const clap_process_t* pr) {
  if (!pr->transport) return;
  gProbe.transport = *pr->transport;
  gProbe.hadTransport = true;
  ++gProbe.transports;
  gProbe.lastFrames = pr->frames_count;
}

// ---------------------------------------------------------------- synth

void synthParam(Synth* s, clap_id id, double v) {
  if (id == kGain) s->gain = v;
  if (id == kWave) s->wave = v;
  if (id == kMode) s->mode = v;
  if (id == kEcho) s->echo = v;
}

void echoNote(const clap_event_note_t* n, const clap_output_events_t* out) {
  clap_event_note_t o = *n;
  o.header.size = sizeof o;
  o.port_index = 0;
  o.key = n->key + 12 <= 127 ? n->key + 12 : n->key;
  o.note_id = -1;
  out->try_push(out, &o.header);
}

void synthEvent(Synth* s, const clap_event_header_t* h, const clap_output_events_t* out) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
      auto* n = reinterpret_cast<const clap_event_note_t*>(h);
      if (n->key == 1) std::abort();  // crash on purpose (sandbox tests)
      s->noteId = n->note_id;
      s->key = n->key;
      s->tuning = s->expr = 0;
      s->amp = n->velocity;
      s->phase = s->subPhase = 0;
      ++s->notes;
      if (s->echo.load() >= 0.5 && out) echoNote(n, out);
      break;
    }
    case CLAP_EVENT_NOTE_OFF: {
      auto* n = reinterpret_cast<const clap_event_note_t*>(h);
      ++gProbe.noteOffs;
      if (n->note_id == s->noteId || n->note_id == -1) s->amp = 0;
      if (s->echo.load() >= 0.5 && out) echoNote(n, out);
      break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
      auto* e = reinterpret_cast<const clap_event_note_expression_t*>(h);
      if (e->expression_id == CLAP_NOTE_EXPRESSION_TUNING && (e->note_id == s->noteId || e->note_id == -1)) s->expr = e->value;
      break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
      auto* p = reinterpret_cast<const clap_event_param_value_t*>(h);
      synthParam(s, p->param_id, p->value);
      break;
    }
    case CLAP_EVENT_PARAM_MOD: {
      auto* m = reinterpret_cast<const clap_event_param_mod_t*>(h);
      if (m->param_id == kGain) s->mod = gProbe.lastMod = m->amount;
      break;
    }
    case CLAP_EVENT_MIDI: {
      auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
      const int st = m->data[0] & 0xF0;
      if (st == 0xE0) s->bend = (((m->data[1] & 0x7F) | ((m->data[2] & 0x7F) << 7)) - 8192) / 8192.0 * 2.0;
      if (st == 0x90 && m->data[2] > 0) {
        s->key = m->data[1];
        s->amp = m->data[2] / 127.0;
        s->noteId = -1;
        ++s->notes;
      }
      if (st == 0x80 || (st == 0x90 && m->data[2] == 0)) s->amp = 0;
      if (st == 0xB0 && m->data[1] == 123) ++gProbe.allNotesOff, s->amp = 0;
      if (st == 0xB0 && m->data[1] == 120) ++gProbe.allSoundOff, s->amp = 0;
      if (st == 0xB0 && m->data[1] == 64 && m->data[2] == 0) ++gProbe.sustainOff;
      if (st == 0xB0 && m->data[1] == 20 && out) {  // behave like an edit in the plugin's own editor
        clap_event_param_gesture_t g{};
        g.header = {sizeof g, h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0};
        g.param_id = kGain;
        out->try_push(out, &g.header);
        clap_event_param_value_t v{};
        v.header = {sizeof v, h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = kGain;
        v.note_id = -1;
        v.port_index = v.channel = v.key = -1;
        v.value = (m->data[2] & 0x7F) / 127.0;
        s->gain = v.value;
        out->try_push(out, &v.header);
        g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &g.header);
      }
      break;
    }
    default:
      break;
  }
}

clap_process_status synthProcess(const clap_plugin_t* p, const clap_process_t* pr) {
  Synth* s = synthOf(p);
  noteTransport(pr);
  const uint32_t n = pr->frames_count;
  const uint32_t ne = pr->in_events->size(pr->in_events);
  uint32_t ei = 0;
  float* L = pr->audio_outputs_count > 0 ? pr->audio_outputs[0].data32[0] : nullptr;
  float* R = pr->audio_outputs_count > 0 ? pr->audio_outputs[0].data32[1] : nullptr;
  float* SL = pr->audio_outputs_count > 1 ? pr->audio_outputs[1].data32[0] : nullptr;
  float* SR = pr->audio_outputs_count > 1 ? pr->audio_outputs[1].data32[1] : nullptr;
  for (uint32_t i = 0; i < n; ++i) {
    while (ei < ne) {
      const clap_event_header_t* h = pr->in_events->get(pr->in_events, ei);
      if (h->time > i) break;
      synthEvent(s, h, pr->out_events);
      ++ei;
    }
    float o = 0, sub = 0;
    if (s->amp > 0) {
      double f = 440.0 * std::pow(2.0, (s->key + s->tuning + s->expr + s->bend - 69.0) / 12.0);
      s->phase += f / s->sr;
      s->phase -= std::floor(s->phase);
      s->subPhase += 0.5 * f / s->sr;
      s->subPhase -= std::floor(s->subPhase);
      double x = std::sin(2.0 * M_PI * s->phase);
      if (s->wave.load() >= 0.5) x = x >= 0 ? 1.0 : -1.0;
      const double g = std::fmin(1.0, std::fmax(0.0, s->gain.load() + s->mod));
      o = static_cast<float>(x * s->amp * g);
      sub = static_cast<float>(std::sin(2.0 * M_PI * s->subPhase) * s->amp * g);
    }
    if (L) L[i] = R[i] = o;
    if (SL) SL[i] = SR[i] = sub;
  }
  while (ei < ne) synthEvent(s, pr->in_events->get(pr->in_events, ei++), pr->out_events);
  return CLAP_PROCESS_CONTINUE;
}

uint32_t synthParamCount(const clap_plugin_t*) { return 4; }
bool synthParamInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
  if (i > 3) return false;
  std::memset(info, 0, sizeof *info);
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  if (i == 0) {
    info->id = kGain;
    setName(info->name, "Gain");
    info->min_value = 0;
    info->max_value = 1;
    info->default_value = 0.5;
    info->flags |= CLAP_PARAM_IS_MODULATABLE;
  } else if (i == 1) {
    info->id = kWave;
    setName(info->name, "Square");
    info->flags |= CLAP_PARAM_IS_STEPPED;
    info->max_value = 1;
  } else if (i == 2) {
    info->id = kMode;
    setName(info->name, "Mode");
    info->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
    info->max_value = 2;
  } else {
    info->id = kEcho;
    setName(info->name, "Echo");
    info->flags |= CLAP_PARAM_IS_STEPPED;
    info->max_value = 1;
  }
  return true;
}
bool synthParamValue(const clap_plugin_t* p, clap_id id, double* v) {
  Synth* s = synthOf(p);
  if (id == kGain) *v = s->gain;
  else if (id == kWave) *v = s->wave;
  else if (id == kMode) *v = s->mode;
  else if (id == kEcho) *v = s->echo;
  else return false;
  return true;
}
bool synthParamText(const clap_plugin_t*, clap_id id, double v, char* out, uint32_t cap) {
  static const char* const modes[] = {"Soft", "Hard", "Fold"};
  if (id == kWave) std::snprintf(out, cap, "%s", v >= 0.5 ? "Square" : "Sine");
  else if (id == kMode) std::snprintf(out, cap, "%s", modes[std::lround(std::fmin(std::fmax(v, 0.0), 2.0))]);
  else if (id == kEcho) std::snprintf(out, cap, "%s", v >= 0.5 ? "On" : "Off");
  else std::snprintf(out, cap, "%.2f x", v);
  return true;
}
bool synthTextParam(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void synthFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
  for (uint32_t i = 0; i < in->size(in); ++i) synthEvent(synthOf(p), in->get(in, i), out);
}
const clap_plugin_params_t kSynthParams = {synthParamCount, synthParamInfo, synthParamValue, synthParamText, synthTextParam, synthFlush};

bool synthSave(const clap_plugin_t* p, const clap_ostream_t* st) {
  Synth* s = synthOf(p);
  const double g = s->gain, w = s->wave, m = s->mode;
  const int32_t n = s->notes;
  return writeAll(st, g) && writeAll(st, w) && writeAll(st, m) && writeAll(st, n);
}
bool synthLoad(const clap_plugin_t* p, const clap_istream_t* st) {
  Synth* s = synthOf(p);
  double g, w, m;
  int32_t n;
  if (!readAll(st, g) || !readAll(st, w) || !readAll(st, m) || !readAll(st, n)) return false;
  s->gain = g;
  s->wave = w;
  s->mode = m;
  s->notes = n;
  return true;
}
const clap_plugin_state_t kSynthState = {synthSave, synthLoad};

uint32_t synthPortCount(const clap_plugin_t*, bool input) { return input ? 0 : 2; }
bool synthPortGet(const clap_plugin_t*, uint32_t i, bool input, clap_audio_port_info_t* info) {
  if (input || i > 1) return false;
  info->id = i;
  setName(info->name, i == 0 ? "Out" : "Sub");
  info->flags = i == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kSynthPorts = {synthPortCount, synthPortGet};

uint32_t noteCount(const clap_plugin_t*, bool) { return 1; }
bool noteGet(const clap_plugin_t*, uint32_t i, bool input, clap_note_port_info_t* info) {
  if (i != 0) return false;
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  setName(info->name, input ? "Notes" : "Echo");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {noteCount, noteGet};

bool renderHard(const clap_plugin_t*) { return false; }
bool renderSet(const clap_plugin_t*, clap_plugin_render_mode m) {
  gProbe.renderMode = m;
  return true;
}
const clap_plugin_render_t kRender = {renderHard, renderSet};

bool presetFromLocation(const clap_plugin_t* p, uint32_t kind, const char*, const char* key) {
  if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !key) return false;
  Synth* s = synthOf(p);
  if (!std::strcmp(key, "soft")) s->gain = 0.2;
  else if (!std::strcmp(key, "loud")) s->gain = 0.9;
  else return false;
  return true;
}
const clap_plugin_preset_load_t kPresetLoad = {presetFromLocation};

#if defined(__APPLE__)
// The synth's own interface (macOS): an embedded, fixed 320 x 200 Cocoa view.
constexpr uint32_t kGuiW = 320, kGuiH = 200;
const clap_plugin_gui_t kSynthGui = {
    [](const clap_plugin_t*, const char* api, bool floating) { return !floating && api && !std::strcmp(api, CLAP_WINDOW_API_COCOA); },
    [](const clap_plugin_t*, const char** api, bool* floating) {
      *api = CLAP_WINDOW_API_COCOA;
      *floating = false;
      return true;
    },
    [](const clap_plugin_t* p, const char* api, bool floating) {
      if (floating || !api || std::strcmp(api, CLAP_WINDOW_API_COCOA) || synthOf(p)->gui) return false;
      synthOf(p)->gui = true;
      return true;
    },
    [](const clap_plugin_t* p) {
      Synth* s = synthOf(p);
      if (s->view) {
        gridlineTestViewRemove(s->view);
        s->view = nullptr;
        --gProbe.views;
      }
      s->gui = false;
    },
    [](const clap_plugin_t*, double) { return false; },
    [](const clap_plugin_t*, uint32_t* w, uint32_t* h) {
      *w = kGuiW;
      *h = kGuiH;
      return true;
    },
    [](const clap_plugin_t*) { return false; },
    [](const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; },
    [](const clap_plugin_t*, uint32_t* w, uint32_t* h) {
      *w = kGuiW;
      *h = kGuiH;
      return true;
    },
    [](const clap_plugin_t*, uint32_t w, uint32_t h) { return w == kGuiW && h == kGuiH; },
    [](const clap_plugin_t* p, const clap_window_t* win) {
      Synth* s = synthOf(p);
      if (!s->gui || s->view || !win || !win->cocoa) return false;
      s->view = gridlineTestViewAttach(win->cocoa, kGuiW, kGuiH, "CLAP test interface", 0.18f, 0.48f, 0.30f);
      if (s->view) ++gProbe.views;
      return s->view != nullptr;
    },
    [](const clap_plugin_t*, const clap_window_t*) { return false; },
    [](const clap_plugin_t*, const char*) {},
    [](const clap_plugin_t*) { return true; },
    [](const clap_plugin_t*) { return true; },
};
#endif

const void* synthExt(const clap_plugin_t*, const char* id) {
#if defined(__APPLE__)
  if (!std::strcmp(id, CLAP_EXT_GUI)) return &kSynthGui;
#endif
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kSynthParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kSynthState;
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kSynthPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_RENDER)) return &kRender;
  if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &kPresetLoad;
  return nullptr;
}

const clap_plugin_t* makeSynth(const clap_host_t* host) {
  Synth* s = new Synth;
  s->host = host;
  clap_plugin_t& p = s->plugin;
  p.desc = &kSynthDesc;
  p.plugin_data = s;
  p.init = [](const clap_plugin_t*) { return true; };
  p.destroy = [](const clap_plugin_t* pl) { delete synthOf(pl); };
  p.activate = [](const clap_plugin_t* pl, double sr, uint32_t, uint32_t) {
    synthOf(pl)->sr = sr;
    return true;
  };
  p.deactivate = [](const clap_plugin_t*) {};
  p.start_processing = [](const clap_plugin_t*) { return true; };
  p.stop_processing = [](const clap_plugin_t*) {};
  p.reset = [](const clap_plugin_t* pl) { synthOf(pl)->amp = 0; };
  p.process = synthProcess;
  p.get_extension = synthExt;
  p.on_main_thread = [](const clap_plugin_t*) {};
  return &s->plugin;
}

// ---------------------------------------------------------------- gain

clap_process_status gainProcess(const clap_plugin_t* p, const clap_process_t* pr) {
  Gain* g = gainOf(p);
  noteTransport(pr);
  if (pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
  const clap_audio_buffer_t& in = pr->audio_inputs[0];
  const clap_audio_buffer_t* sc = pr->audio_inputs_count > 1 ? &pr->audio_inputs[1] : nullptr;
  const clap_audio_buffer_t& out = pr->audio_outputs[0];
  const uint32_t ne = pr->in_events->size(pr->in_events);
  uint32_t ei = 0;
  const int lat = g->latency.load();
  for (uint32_t i = 0; i < pr->frames_count; ++i) {
    for (; ei < ne; ++ei) {  // sample-accurate parameter changes
      const clap_event_header_t* h = pr->in_events->get(pr->in_events, ei);
      if (h->time > i) break;
      if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
      auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
      if (v->param_id == kLevel) {
        g->level = v->value;
        gProbe.lastParamTime = static_cast<int>(h->time);
      }
      if (v->param_id == kSide) g->side = v->value;
      if (v->param_id == kBypass) g->bypass = v->value;
      if (v->param_id == kLong && (v->value >= 0.5) != (g->longMode.load() >= 0.5)) {
        g->longMode = v->value;
        g->host->request_restart(g->host);  // the new latency applies after a restart
      }
    }
    const bool bypassed = g->bypass.load() >= 0.5;
    for (uint32_t c = 0; c < 2 && c < out.channel_count; ++c) {
      float x = in.data32[c][i];
      if (!bypassed) {
        x *= static_cast<float>(g->level.load());
        if (sc && c < sc->channel_count) x += sc->data32[c][i] * static_cast<float>(g->side.load());
      }
      // `lat` samples of latency (reported through clap.latency).
      g->delay[c][g->pos & 63] = x;
      out.data32[c][i] = g->delay[c][(g->pos - lat) & 63];
    }
    ++g->pos;
  }
  return CLAP_PROCESS_CONTINUE;
}
uint32_t gainParamCount(const clap_plugin_t*) { return 4; }
bool gainParamInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
  if (i > 3) return false;
  std::memset(info, 0, sizeof *info);
  static const clap_id ids[] = {kLevel, kSide, kLong, kBypass};
  static const char* const names[] = {"Level", "Sidechain", "Long", "Bypass"};
  info->id = ids[i];
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  setName(info->name, names[i]);
  info->min_value = 0;
  info->max_value = i == 0 ? 2.0 : 1.0;
  info->default_value = i == 0 ? 1.0 : 0.0;
  if (i >= 2) info->flags |= CLAP_PARAM_IS_STEPPED;
  if (i == 3) info->flags |= CLAP_PARAM_IS_BYPASS;
  return true;
}
bool gainParamValue(const clap_plugin_t* p, clap_id id, double* v) {
  Gain* g = gainOf(p);
  if (id == kLevel) *v = g->level;
  else if (id == kSide) *v = g->side;
  else if (id == kLong) *v = g->longMode;
  else if (id == kBypass) *v = g->bypass;
  else return false;
  return true;
}
bool gainParamText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t cap) {
  std::snprintf(out, cap, "%.2f", v);
  return true;
}
void gainFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
  Gain* g = gainOf(p);
  for (uint32_t i = 0; i < in->size(in); ++i) {
    const clap_event_header_t* h = in->get(in, i);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
    auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
    if (v->param_id == kLevel) g->level = v->value;
    if (v->param_id == kSide) g->side = v->value;
    if (v->param_id == kLong) g->longMode = v->value;
    if (v->param_id == kBypass) g->bypass = v->value;
  }
}
const clap_plugin_params_t kGainParams = {gainParamCount, gainParamInfo, gainParamValue, gainParamText, synthTextParam, gainFlush};
bool gainSave(const clap_plugin_t* p, const clap_ostream_t* st) {
  const double l = gainOf(p)->level, s = gainOf(p)->side;
  return writeAll(st, l) && writeAll(st, s);
}
bool gainLoad(const clap_plugin_t* p, const clap_istream_t* st) {
  double l, s;
  if (!readAll(st, l) || !readAll(st, s)) return false;
  gainOf(p)->level = l;
  gainOf(p)->side = s;
  return true;
}
const clap_plugin_state_t kGainState = {gainSave, gainLoad};
uint32_t gainPortCount(const clap_plugin_t*, bool input) { return input ? 2 : 1; }
bool gainPortGet(const clap_plugin_t*, uint32_t i, bool input, clap_audio_port_info_t* info) {
  if (i > (input ? 1u : 0u)) return false;
  info->id = i;
  setName(info->name, i == 0 ? "Main" : "Sidechain");
  info->flags = i == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kGainPorts = {gainPortCount, gainPortGet};
uint32_t gainLatency(const clap_plugin_t* p) { return static_cast<uint32_t>(gainOf(p)->latency.load()); }
const clap_plugin_latency_t kGainLatency = {gainLatency};
const void* gainExt(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kGainParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kGainState;
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kGainLatency;
  return nullptr;
}
const clap_plugin_t* makeGain(const clap_host_t* host) {
  Gain* g = new Gain;
  g->host = host;
  clap_plugin_t& p = g->plugin;
  p.desc = &kGainDesc;
  p.plugin_data = g;
  p.init = [](const clap_plugin_t*) { return true; };
  p.destroy = [](const clap_plugin_t* pl) { delete gainOf(pl); };
  p.activate = [](const clap_plugin_t* pl, double, uint32_t, uint32_t) {
    Gain* gg = gainOf(pl);
    const int want = gg->longMode.load() >= 0.5 ? 64 : 16;
    if (want != gg->latency.load()) ++gProbe.latencyRestarts;
    gg->latency = want;
    return true;
  };
  p.deactivate = [](const clap_plugin_t*) {};
  p.start_processing = [](const clap_plugin_t*) { return true; };
  p.stop_processing = [](const clap_plugin_t*) {};
  p.reset = [](const clap_plugin_t*) {};
  p.process = gainProcess;
  p.get_extension = gainExt;
  p.on_main_thread = [](const clap_plugin_t*) {};
  return &g->plugin;
}

// ---------------------------------------------------------------- preset discovery

const clap_preset_discovery_provider_descriptor_t kProviderDesc = {CLAP_VERSION_INIT, "org.gridline.test.presets", "Gridline test presets",
                                                                   "Gridline Tests"};
const clap_preset_discovery_provider_t* createProvider(const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
                                                       const char* id) {
  if (std::strcmp(id, kProviderDesc.id) != 0) return nullptr;
  struct Prov {
    clap_preset_discovery_provider_t p;
    const clap_preset_discovery_indexer_t* ix;
  };
  Prov* pv = new Prov;
  pv->ix = indexer;
  pv->p.desc = &kProviderDesc;
  pv->p.provider_data = pv;
  pv->p.init = [](const clap_preset_discovery_provider_t* p) {
    auto* self = static_cast<Prov*>(p->provider_data);
    clap_preset_discovery_location_t loc{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
    return self->ix->declare_location(self->ix, &loc);
  };
  pv->p.destroy = [](const clap_preset_discovery_provider_t* p) { delete static_cast<Prov*>(p->provider_data); };
  pv->p.get_metadata = [](const clap_preset_discovery_provider_t*, uint32_t kind, const char*, const clap_preset_discovery_metadata_receiver_t* r) {
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) return false;
    const clap_universal_plugin_id_t synth{"clap", "org.gridline.test.synth"};
    const char* names[] = {"Soft Sine", "Loud Sine"};
    const char* keys[] = {"soft", "loud"};
    for (int i = 0; i < 2; ++i) {
      if (!r->begin_preset(r, names[i], keys[i])) return true;
      r->add_plugin_id(r, &synth);
      r->add_creator(r, "Gridline Tests");
      r->add_feature(r, "Lead");
    }
    return true;
  };
  pv->p.get_extension = [](const clap_preset_discovery_provider_t*, const char*) -> const void* { return nullptr; };
  return &pv->p;
}
uint32_t providerCount(const clap_preset_discovery_factory_t*) { return 1; }
const clap_preset_discovery_provider_descriptor_t* providerDesc(const clap_preset_discovery_factory_t*, uint32_t i) {
  return i == 0 ? &kProviderDesc : nullptr;
}
const clap_preset_discovery_factory_t kPresetFactory = {providerCount, providerDesc, createProvider};

// ---------------------------------------------------------------- factory / entry

uint32_t count(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kSynthDesc : i == 1 ? &kGainDesc : nullptr; }
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!std::strcmp(id, kSynthDesc.id)) return makeSynth(host);
  if (!std::strcmp(id, kGainDesc.id)) return makeGain(host);
  return nullptr;
}
const clap_plugin_factory_t kFactory = {count, descriptor, create};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) {
  if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
  if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID)) return &kPresetFactory;
  return nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
extern "C" CLAP_EXPORT GridlineClapProbe* gridline_clap_probe() { return &gProbe; }

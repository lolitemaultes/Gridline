// Gridline — minimal LV2 plugins used by the host tests (built in-tree into
// test-lv2/GridlineTest.lv2; never installed). Written against the LV2
// specification headers only.
//   urn:gridline:lv2test:gain   effect: stereo gain (sample-accurate through
//       the host splitting run()), mode (enumeration), invert (toggle),
//       frequency (logarithmic, display only), echo (written by its
//       interface), side (mixes in the sidechain input), lv2:enabled bypass,
//       level meter (control output). Presets "Quiet" / "Loud" (and "Extra"
//       from a second bundle). Cocoa interface on macOS.
//   urn:gridline:lv2test:synth  instrument: one sine voice from atom MIDI
//       input, time:Position recorded in the probe, MIDI output echoing
//       every note one octave higher when "echo" is on.
//   urn:gridline:lv2test:state  mono effect: 32 samples latency (latency
//       port), a worker that "loads" a scale factor when "trigger" rises, the
//       state extension (scale, a name, a path), freeWheeling port; external
//       (kx) interface.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include "Probe.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/core/lv2.h"
#include "lv2/core/lv2_util.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/state/state.h"
#include "lv2/time/time.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"

namespace {

GridlineLv2Probe gProbe;

const LV2_Feature* feature(const LV2_Feature* const* f, const char* uri) {
  for (; f && *f; ++f)
    if (!std::strcmp((*f)->URI, uri)) return *f;
  return nullptr;
}

// ---------------------------------------------------------------- gain

struct Gain {
  const float* in[2] = {nullptr, nullptr};
  float* out[2] = {nullptr, nullptr};
  const float* sc = nullptr;
  const float* gain = nullptr;
  const float* mode = nullptr;
  const float* invert = nullptr;
  const float* freq = nullptr;
  const float* echo = nullptr;
  const float* enabled = nullptr;
  const float* side = nullptr;
  float* level = nullptr;
};

LV2_Handle gainNew(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*) { return new (std::nothrow) Gain; }
void gainConnect(LV2_Handle h, uint32_t port, void* data) {
  auto* g = static_cast<Gain*>(h);
  switch (port) {
    case 0: g->in[0] = static_cast<const float*>(data); break;
    case 1: g->in[1] = static_cast<const float*>(data); break;
    case 2: g->out[0] = static_cast<float*>(data); break;
    case 3: g->out[1] = static_cast<float*>(data); break;
    case 4: g->gain = static_cast<const float*>(data); break;
    case 5: g->mode = static_cast<const float*>(data); break;
    case 6: g->invert = static_cast<const float*>(data); break;
    case 7: g->freq = static_cast<const float*>(data); break;
    case 8: g->echo = static_cast<const float*>(data); break;
    case 9: g->enabled = static_cast<const float*>(data); break;
    case 10: g->level = static_cast<float*>(data); break;
    case 11: g->side = static_cast<const float*>(data); break;
    case 12: g->sc = static_cast<const float*>(data); break;
    default: break;
  }
}
void gainRun(LV2_Handle h, uint32_t n) {
  auto* g = static_cast<Gain*>(h);
  const bool on = *g->enabled >= 0.5f;
  const float k = (*g->invert >= 0.5f ? -1.0f : 1.0f) * *g->gain;
  float peak = 0;
  for (int c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < n; ++i) {
      float v = on ? g->in[c][i] * k + (g->sc ? g->sc[i] * *g->side : 0.0f) : g->in[c][i];
      g->out[c][i] = v;
      peak = std::fmax(peak, std::fabs(v));
    }
  *g->level = peak;
}
void gainFree(LV2_Handle h) { delete static_cast<Gain*>(h); }

// ---------------------------------------------------------------- synth

struct Synth {
  const LV2_Atom_Sequence* events = nullptr;
  LV2_Atom_Sequence* midiOut = nullptr;
  float* out[2] = {nullptr, nullptr};
  const float* gain = nullptr;
  const float* echo = nullptr;
  double sr = 48000;
  int key = 60;
  double amp = 0, phase = 0;
  LV2_URID midiEvent = 0, position = 0, frame = 0, speed = 0, bpm = 0, bar = 0, barBeat = 0, beat = 0, beatUnit = 0, beatsPerBar = 0,
           atomLong = 0, atomFloat = 0, atomInt = 0, atomDouble = 0, atomObject = 0, atomBlank = 0;
  LV2_Atom_Forge forge;
};

LV2_Handle synthNew(const LV2_Descriptor*, double sr, const char*, const LV2_Feature* const* features) {
  const LV2_Feature* f = feature(features, LV2_URID__map);
  if (!f) return nullptr;
  auto* map = static_cast<LV2_URID_Map*>(f->data);
  auto* s = new (std::nothrow) Synth;
  if (!s) return nullptr;
  s->sr = sr;
  auto m = [map](const char* u) { return map->map(map->handle, u); };
  s->midiEvent = m(LV2_MIDI__MidiEvent);
  s->position = m(LV2_TIME__Position);
  s->frame = m(LV2_TIME__frame);
  s->speed = m(LV2_TIME__speed);
  s->bpm = m(LV2_TIME__beatsPerMinute);
  s->bar = m(LV2_TIME__bar);
  s->barBeat = m(LV2_TIME__barBeat);
  s->beat = m(LV2_TIME__beat);
  s->beatUnit = m(LV2_TIME__beatUnit);
  s->beatsPerBar = m(LV2_TIME__beatsPerBar);
  s->atomLong = m(LV2_ATOM__Long);
  s->atomFloat = m(LV2_ATOM__Float);
  s->atomInt = m(LV2_ATOM__Int);
  s->atomDouble = m(LV2_ATOM__Double);
  s->atomObject = m(LV2_ATOM__Object);
  s->atomBlank = m(LV2_ATOM__Blank);
  lv2_atom_forge_init(&s->forge, map);
  return s;
}
void synthConnect(LV2_Handle h, uint32_t port, void* data) {
  auto* s = static_cast<Synth*>(h);
  switch (port) {
    case 0: s->events = static_cast<const LV2_Atom_Sequence*>(data); break;
    case 1: s->out[0] = static_cast<float*>(data); break;
    case 2: s->out[1] = static_cast<float*>(data); break;
    case 3: s->gain = static_cast<const float*>(data); break;
    case 4: s->echo = static_cast<const float*>(data); break;
    case 5: s->midiOut = static_cast<LV2_Atom_Sequence*>(data); break;
    default: break;
  }
}

void readPosition(Synth* s, const LV2_Atom_Object* obj) {
  ++gProbe.positions;
  LV2_ATOM_OBJECT_FOREACH(obj, p) {
    const LV2_Atom* v = &p->value;
    if (p->key == s->frame && v->type == s->atomLong) gProbe.frame = reinterpret_cast<const LV2_Atom_Long*>(v)->body;
    else if (p->key == s->speed && v->type == s->atomFloat) gProbe.speed = reinterpret_cast<const LV2_Atom_Float*>(v)->body;
    else if (p->key == s->bpm && v->type == s->atomFloat) gProbe.bpm = reinterpret_cast<const LV2_Atom_Float*>(v)->body;
    else if (p->key == s->bar && v->type == s->atomLong) gProbe.bar = reinterpret_cast<const LV2_Atom_Long*>(v)->body;
    else if (p->key == s->barBeat && v->type == s->atomFloat) gProbe.barBeat = reinterpret_cast<const LV2_Atom_Float*>(v)->body;
    else if (p->key == s->beat && v->type == s->atomDouble) gProbe.beat = reinterpret_cast<const LV2_Atom_Double*>(v)->body;
    else if (p->key == s->beatUnit && v->type == s->atomInt) gProbe.beatUnit = reinterpret_cast<const LV2_Atom_Int*>(v)->body;
    else if (p->key == s->beatsPerBar && v->type == s->atomFloat) gProbe.beatsPerBar = reinterpret_cast<const LV2_Atom_Float*>(v)->body;
  }
}

void synthRun(LV2_Handle h, uint32_t n) {
  auto* s = static_cast<Synth*>(h);
  const uint32_t cap = s->midiOut->atom.size;
  lv2_atom_forge_set_buffer(&s->forge, reinterpret_cast<uint8_t*>(s->midiOut), cap + sizeof(LV2_Atom));
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_sequence_head(&s->forge, &frame, 0);
  uint32_t pos = 0;
  auto render = [s](uint32_t from, uint32_t to) {
    const double inc = 440.0 * std::pow(2.0, (s->key - 69) / 12.0) / s->sr;
    for (uint32_t i = from; i < to; ++i) {
      float v = static_cast<float>(std::sin(6.283185307179586 * s->phase) * s->amp * *s->gain);
      s->phase += inc;
      if (s->phase >= 1) s->phase -= 1;
      s->out[0][i] = v;
      s->out[1][i] = v;
    }
  };
  LV2_ATOM_SEQUENCE_FOREACH(s->events, ev) {
    const uint32_t t = static_cast<uint32_t>(ev->time.frames);
    if (t > pos && t <= n) {
      render(pos, t);
      pos = t;
    }
    if (ev->body.type == s->midiEvent) {
      const uint8_t* m = static_cast<const uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
      const uint8_t st = m[0] & 0xF0;
      if (st == 0x90 && m[2] > 0) {
        if (m[1] == 1) std::abort();  // crash on purpose (sandbox tests)
        s->key = m[1];
        s->amp = m[2] / 127.0;
        s->phase = 0.25;  // starts at its peak: the first sample of the note is not zero
        ++gProbe.notesOn;
      } else if (st == 0x80 || (st == 0x90 && m[2] == 0)) {
        if (m[1] == s->key) s->amp = 0;
      }
      if ((st == 0x80 || st == 0x90) && *s->echo >= 0.5f) {
        uint8_t o[3] = {m[0], static_cast<uint8_t>(m[1] + 12 <= 127 ? m[1] + 12 : m[1]), m[2]};
        lv2_atom_forge_frame_time(&s->forge, ev->time.frames);
        lv2_atom_forge_atom(&s->forge, 3, s->midiEvent);
        lv2_atom_forge_write(&s->forge, o, 3);
      }
    } else if (ev->body.type == s->atomObject || ev->body.type == s->atomBlank) {
      const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
      if (obj->body.otype == s->position) readPosition(s, obj);
    }
  }
  render(pos, n);
  lv2_atom_forge_pop(&s->forge, &frame);
}
void synthFree(LV2_Handle h) { delete static_cast<Synth*>(h); }

// ---------------------------------------------------------------- state / worker / latency

struct Stateful {
  const float* in = nullptr;
  float* out = nullptr;
  const float* trigger = nullptr;
  float* latency = nullptr;
  const float* freewheel = nullptr;
  const LV2_Atom_Sequence* control = nullptr;
  LV2_Atom_Sequence* notify = nullptr;
  float delay[32] = {};
  int pos = 0;
  float scale = 1.0f;
  bool lastTrigger = false;
  int counter = 0;
  std::string name = "fresh";
  std::string path;
  LV2_Worker_Schedule* schedule = nullptr;
  LV2_URID kScale = 0, kName = 0, kPath = 0, atomFloat = 0, atomString = 0, atomPath = 0, atomInt = 0, atomChunk = 0, atomSequence = 0;
  std::atomic<bool> inRun{false};
  int sentCounter = 0;
  LV2_Atom_Forge forge;
};

LV2_Handle stateNew(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
  const LV2_Feature* fm = feature(features, LV2_URID__map);
  const LV2_Feature* fw = feature(features, LV2_WORKER__schedule);
  if (!fm || !fw) return nullptr;
  auto* map = static_cast<LV2_URID_Map*>(fm->data);
  auto* s = new (std::nothrow) Stateful;
  if (!s) return nullptr;
  s->schedule = static_cast<LV2_Worker_Schedule*>(fw->data);
  auto m = [map](const char* u) { return map->map(map->handle, u); };
  s->kScale = m("urn:gridline:lv2test:state#scale");
  s->kName = m("urn:gridline:lv2test:state#name");
  s->kPath = m("urn:gridline:lv2test:state#path");
  s->atomFloat = m(LV2_ATOM__Float);
  s->atomString = m(LV2_ATOM__String);
  s->atomPath = m(LV2_ATOM__Path);
  s->atomInt = m(LV2_ATOM__Int);
  s->atomChunk = m(LV2_ATOM__Chunk);
  s->atomSequence = m(LV2_ATOM__Sequence);
  lv2_atom_forge_init(&s->forge, map);
  if (const LV2_Feature* fo = feature(features, LV2_OPTIONS__options)) {
    const LV2_URID maxBlock = m(LV2_BUF_SIZE__maxBlockLength);
    for (auto* o = static_cast<const LV2_Options_Option*>(fo->data); o && o->key; ++o)
      if (o->key == maxBlock && o->type == s->atomInt) gProbe.maxBlock = *static_cast<const int32_t*>(o->value);
  }
  return s;
}
void stateConnect(LV2_Handle h, uint32_t port, void* data) {
  auto* s = static_cast<Stateful*>(h);
  switch (port) {
    case 0: s->in = static_cast<const float*>(data); break;
    case 1: s->out = static_cast<float*>(data); break;
    case 2: s->trigger = static_cast<const float*>(data); break;
    case 3: s->latency = static_cast<float*>(data); break;
    case 4: s->freewheel = static_cast<const float*>(data); break;
    case 5: s->control = static_cast<const LV2_Atom_Sequence*>(data); break;
    case 6: s->notify = static_cast<LV2_Atom_Sequence*>(data); break;
    default: break;
  }
}
void stateRun(LV2_Handle h, uint32_t n) {
  auto* s = static_cast<Stateful*>(h);
  s->inRun = true;
  ++gProbe.runs;
  gProbe.freewheel = *s->freewheel;
  *s->latency = 32;
  const bool trig = *s->trigger >= 0.5f;
  if (trig && !s->lastTrigger) {
    int32_t req = ++s->counter;
    s->schedule->schedule_work(s->schedule->handle, sizeof req, &req);
  }
  s->lastTrigger = trig;
  // Messages from the interface: an Int atom adds to the counter.
  LV2_ATOM_SEQUENCE_FOREACH(s->control, ev) {
    if (ev->body.type == s->atomInt) s->counter += reinterpret_cast<const LV2_Atom_Int*>(&ev->body)->body;
  }
  for (uint32_t i = 0; i < n; ++i) {
    s->out[i] = s->delay[s->pos] * s->scale;
    s->delay[s->pos] = s->in[i];
    s->pos = (s->pos + 1) % 32;
  }
  // Notify output: the counter (an Int) whenever it changed.
  lv2_atom_forge_set_buffer(&s->forge, reinterpret_cast<uint8_t*>(s->notify), s->notify->atom.size + sizeof(LV2_Atom));
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_sequence_head(&s->forge, &frame, 0);
  if (s->counter != s->sentCounter) {
    s->sentCounter = s->counter;
    lv2_atom_forge_frame_time(&s->forge, 0);
    lv2_atom_forge_int(&s->forge, s->counter);
  }
  lv2_atom_forge_pop(&s->forge, &frame);
  s->inRun = false;
}
void stateFree(LV2_Handle h) { delete static_cast<Stateful*>(h); }

LV2_Worker_Status stateWork(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void* data) {
  auto* s = static_cast<Stateful*>(h);
  ++gProbe.works;
  gProbe.workOnRunThread = s->inRun.load();
  if (size != sizeof(int32_t)) return LV2_WORKER_ERR_UNKNOWN;
  int32_t req = 0;
  std::memcpy(&req, data, sizeof req);
  const float scale = 0.5f / static_cast<float>(req);  // "loaded" data
  return respond(rh, sizeof scale, &scale);
}
LV2_Worker_Status stateWorkResponse(LV2_Handle h, uint32_t size, const void* body) {
  auto* s = static_cast<Stateful*>(h);
  ++gProbe.responses;
  if (size == sizeof(float)) std::memcpy(&s->scale, body, sizeof(float));
  s->name = "loaded:" + std::to_string(s->counter);
  return LV2_WORKER_SUCCESS;
}

LV2_State_Status stateSave(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t, const LV2_Feature* const* features) {
  auto* s = static_cast<Stateful*>(h);
  store(sh, s->kScale, &s->scale, sizeof(float), s->atomFloat, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(sh, s->kName, s->name.c_str(), s->name.size() + 1, s->atomString, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  if (!s->path.empty()) {
    const LV2_Feature* fmp = feature(features, LV2_STATE__mapPath);
    std::string p = s->path;
    if (fmp) {
      auto* mp = static_cast<LV2_State_Map_Path*>(fmp->data);
      char* abs = mp->abstract_path(mp->handle, s->path.c_str());
      p = abs;
      if (const LV2_Feature* ff = feature(features, LV2_STATE__freePath))
        static_cast<LV2_State_Free_Path*>(ff->data)->free_path(static_cast<LV2_State_Free_Path*>(ff->data)->handle, abs);
      else
        std::free(abs);
    }
    store(sh, s->kPath, p.c_str(), p.size() + 1, s->atomPath, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  }
  return LV2_STATE_SUCCESS;
}
LV2_State_Status stateRestore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh, uint32_t, const LV2_Feature* const*) {
  auto* s = static_cast<Stateful*>(h);
  ++gProbe.restores;
  size_t size = 0;
  uint32_t type = 0, flags = 0;
  if (const void* v = retrieve(sh, s->kScale, &size, &type, &flags); v && type == s->atomFloat && size == sizeof(float)) std::memcpy(&s->scale, v, sizeof(float));
  if (const void* v = retrieve(sh, s->kName, &size, &type, &flags); v && type == s->atomString && size > 0)
    s->name.assign(static_cast<const char*>(v), size - 1);
  if (const void* v = retrieve(sh, s->kPath, &size, &type, &flags); v && type == s->atomPath && size > 0)
    s->path.assign(static_cast<const char*>(v), size - 1);
  return LV2_STATE_SUCCESS;
}

const void* stateExtension(const char* uri) {
  static const LV2_Worker_Interface worker = {stateWork, stateWorkResponse, nullptr};
  static const LV2_State_Interface state = {stateSave, stateRestore};
  if (!std::strcmp(uri, LV2_WORKER__interface)) return &worker;
  if (!std::strcmp(uri, LV2_STATE__interface)) return &state;
  return nullptr;
}

const LV2_Descriptor kGainDesc = {"urn:gridline:lv2test:gain", gainNew, gainConnect, nullptr, gainRun, nullptr, gainFree, nullptr};
const LV2_Descriptor kSynthDesc = {"urn:gridline:lv2test:synth", synthNew, synthConnect, nullptr, synthRun, nullptr, synthFree, nullptr};
const LV2_Descriptor kStateDesc = {"urn:gridline:lv2test:state", stateNew, stateConnect, nullptr, stateRun, nullptr, stateFree, stateExtension};

}  // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  switch (index) {
    case 0: return &kGainDesc;
    case 1: return &kSynthDesc;
    case 2: return &kStateDesc;
    default: return nullptr;
  }
}

LV2_SYMBOL_EXPORT GridlineLv2Probe* gridline_lv2_probe() { return &gProbe; }

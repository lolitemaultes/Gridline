// Gridline — minimal VST2-style plugin used by the host tests (built in-tree
// as GridlineTestVst2; never installed), written against Gridline's own
// clean-room ABI description (src/host/vst2/Vst2Abi.h). The binary is a shell
// with two sub-plugins:
//   'GlT2'  instrument: one sine voice, MIDI notes, pitch bend (+-2 st), CC 20
//           reports a Gain edit to the host (as from its editor), three
//           programs, four outputs (main + "Sub", one octave lower);
//           parameter-list state.
//   'GlT3'  effect: level, state as an opaque chunk, soft bypass.
// The synth echoes every note it receives one octave up through the host's
// MIDI output callback, and both record the host's time info and process
// level in a probe the tests read (gridline_vst2_probe()).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "host/vst2/Vst2Abi.h"
#include "Probe.h"

namespace abi = gl::vst2abi;

namespace {

GridlineVst2Probe gProbe;

constexpr int32_t kSynthId = 0x476C5432;  // 'GlT2'
constexpr int32_t kFxId = 0x476C5433;     // 'GlT3'
const double kProgramGain[3] = {0.25, 0.5, 1.0};
const char* const kProgramNames[3] = {"Quiet", "Normal", "Loud"};

struct Plugin {
  abi::Effect fx{};
  abi::HostCallback host = nullptr;
  int kind = 0;  // 0 shell, 1 synth, 2 effect
  double sr = 48000;
  float params[2] = {0.5f, 0.0f};  // synth: gain, square; effect: level
  int program = 1;
  int key = 60;
  double amp = 0, bend = 0, phase = 0, subPhase = 0;
  int shellNext = 0;
  bool bypass = false;
};

Plugin* self(abi::Effect* e) { return static_cast<Plugin*>(e->pluginObject); }

void copy(void* dst, const char* src) { std::strcpy(static_cast<char*>(dst), src); }

void processSynth(Plugin* p, float** out, int32_t n) {
  for (int32_t i = 0; i < n; ++i) {
    float o = 0, sub = 0;
    if (p->amp > 0) {
      double f = 440.0 * std::pow(2.0, (p->key + p->bend - 69.0) / 12.0);
      p->phase += f / p->sr;
      p->phase -= std::floor(p->phase);
      p->subPhase += 0.5 * f / p->sr;
      p->subPhase -= std::floor(p->subPhase);
      double x = std::sin(2.0 * M_PI * p->phase);
      if (p->params[1] >= 0.5f) x = x >= 0 ? 1.0 : -1.0;
      o = static_cast<float>(x * p->amp * p->params[0]);
      sub = static_cast<float>(std::sin(2.0 * M_PI * p->subPhase) * p->amp * p->params[0]);
    }
    out[0][i] = out[1][i] = o;
    out[2][i] = out[3][i] = sub;
  }
}

void processReplacing(abi::Effect* e, float** in, float** out, int32_t n) {
  Plugin* p = self(e);
  if (auto* t = reinterpret_cast<abi::TimeInfo*>(p->host(e, abi::hostGetTime, 0, 0xFFFF, nullptr, 0))) {
    gProbe.time = *t;
    ++gProbe.timeCalls;
  }
  gProbe.processLevel = static_cast<int>(p->host(e, abi::hostGetProcessLevel, 0, 0, nullptr, 0));
  if (p->kind == 2 && p->bypass) {
    for (int c = 0; c < 2; ++c)
      for (int32_t i = 0; i < n; ++i) out[c][i] = in[c][i];
    return;
  }
  if (p->kind == 1) {
    processSynth(p, out, n);
    return;
  }
  for (int c = 0; c < 2; ++c)
    for (int32_t i = 0; i < n; ++i) out[c][i] = in[c][i] * (p->kind == 2 ? p->params[0] * 2.0f : 1.0f);
}

void setParam(abi::Effect* e, int32_t i, float v) {
  if (i >= 0 && i < 2) self(e)->params[i] = v;
}
float getParam(abi::Effect* e, int32_t i) { return i >= 0 && i < 2 ? self(e)->params[i] : 0.0f; }

void midi(Plugin* p, const abi::MidiEvent& m) {
  const int st = m.data[0] & 0xF0;
  if (st == 0x90 || st == 0x80) {  // echo one octave up through the host's MIDI output
    abi::MidiEvent o = m;
    o.data[1] = static_cast<uint8_t>(std::min(127, m.data[1] + 12));
    abi::EventList<1> list{};
    list.count = 1;
    list.events[0] = &o;
    p->host(&p->fx, abi::hostProcessEvents, 0, 0, &list, 0);
  }
  if (st == 0x90 && m.data[2] > 0) {
    p->key = m.data[1];
    p->amp = m.data[2] / 127.0;
    p->phase = p->subPhase = 0;
  } else if (st == 0x80 || (st == 0x90 && m.data[2] == 0)) {
    if (m.data[1] == p->key) p->amp = 0;
  } else if (st == 0xE0) {
    p->bend = (((m.data[1] & 0x7F) | ((m.data[2] & 0x7F) << 7)) - 8192) / 8192.0 * 2.0;
  } else if (st == 0xB0 && m.data[1] == 20) {  // as if turned in the plugin's editor
    float v = (m.data[2] & 0x7F) / 127.0f;
    p->params[0] = v;
    p->host(&p->fx, abi::hostBeginEdit, 0, 0, nullptr, 0);
    p->host(&p->fx, abi::hostAutomate, 0, 0, nullptr, v);
    p->host(&p->fx, abi::hostEndEdit, 0, 0, nullptr, 0);
  }
}

intptr_t dispatch(abi::Effect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
  Plugin* p = self(e);
  switch (op) {
    case abi::opClose:
      delete p;
      return 1;
    case abi::opSetSampleRate:
      p->sr = opt;
      return 1;
    case abi::opGetCategory: return p->kind == 0 ? abi::catShell : p->kind == 1 ? abi::catInstrument : abi::catEffect;
    case abi::opShellNext: {
      if (p->kind != 0) return 0;
      const int k = p->shellNext++;
      if (k == 0) copy(ptr, "Gridline Test VST2 Synth");
      if (k == 1) copy(ptr, "Gridline Test VST2 Level");
      return k == 0 ? kSynthId : k == 1 ? kFxId : 0;
    }
    case abi::opGetName:
      copy(ptr, p->kind == 1 ? "Gridline Test VST2 Synth" : p->kind == 2 ? "Gridline Test VST2 Level" : "Gridline Test VST2 Shell");
      return 1;
    case abi::opGetVendor:
      copy(ptr, "Gridline Tests");
      return 1;
    case abi::opGetProduct:
      copy(ptr, "Gridline Test VST2");
      return 1;
    case abi::opGetVendorVersion: return 100;
    case abi::opGetApiVersion: return 2400;
    case abi::opGetParamName:
      copy(ptr, p->kind == 2 ? "Level" : index == 0 ? "Gain" : "Square");
      return 1;
    case abi::opGetParamUnit:
      copy(ptr, p->kind == 2 || index == 0 ? "x" : "");
      return 1;
    case abi::opGetParamText: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.2f", p->params[index & 1]);
      copy(ptr, buf);
      return 1;
    }
    case abi::opGetProgram: return p->program;
    case abi::opSetProgram:
      if (p->kind == 1 && value >= 0 && value < 3) {
        p->program = static_cast<int>(value);
        p->params[0] = static_cast<float>(kProgramGain[value]);
      }
      return 1;
    case abi::opGetProgramNameIndexed:
      if (p->kind != 1 || index < 0 || index > 2) return 0;
      copy(ptr, kProgramNames[index]);
      return 1;
    case abi::opCanDo: {
      const char* s = static_cast<const char*>(ptr);
      if (!s) return -1;
      if (p->kind == 1 && (!std::strcmp(s, "receiveVstMidiEvent") || !std::strcmp(s, "sendVstMidiEvent"))) return 1;
      if (p->kind == 2 && !std::strcmp(s, "bypass")) return 1;
      return -1;
    }
    case abi::opSetBypass:
      p->bypass = value != 0;
      ++gProbe.bypassCalls;
      return 1;
    case abi::opProcessEvents: {
      auto* list = static_cast<abi::EventList<2>*>(ptr);
      for (int32_t i = 0; i < list->count; ++i) {
        auto* m = static_cast<abi::MidiEvent*>(list->events[i]);
        if (m->type == 1) midi(p, *m);
      }
      return 1;
    }
    case abi::opGetOutputInfo: {
      if (p->kind != 1 || index < 0 || index > 3) return 0;
      auto* pin = static_cast<abi::PinInfo*>(ptr);
      std::memset(pin, 0, sizeof *pin);
      copy(pin->label, index < 2 ? "Out" : "Sub");
      pin->flags = 1 | ((index & 1) == 0 ? 2 : 0);
      return 1;
    }
    case abi::opGetChunk: {
      if (p->kind != 2) return 0;
      static thread_local char chunk[8];
      std::memcpy(chunk, "GLT3", 4);
      std::memcpy(chunk + 4, &p->params[0], 4);
      *static_cast<void**>(ptr) = chunk;
      return 8;
    }
    case abi::opSetChunk: {
      if (p->kind != 2 || value != 8 || std::memcmp(ptr, "GLT3", 4) != 0) return 0;
      std::memcpy(&p->params[0], static_cast<const char*>(ptr) + 4, 4);
      return 1;
    }
    default: return 0;
  }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) GridlineVst2Probe* gridline_vst2_probe() { return &gProbe; }

extern "C" __attribute__((visibility("default"))) abi::Effect* VSTPluginMain(abi::HostCallback host) {
  if (!host) return nullptr;
  const int32_t want = static_cast<int32_t>(host(nullptr, abi::hostCurrentId, 0, 0, nullptr, 0));
  auto* p = new Plugin;
  p->host = host;
  p->kind = want == kSynthId ? 1 : want == kFxId ? 2 : 0;
  abi::Effect& e = p->fx;
  e.magic = abi::kMagic;
  e.dispatch = dispatch;
  e.setParam = setParam;
  e.getParam = getParam;
  e.processReplacing = processReplacing;
  e.pluginObject = p;
  e.flags = abi::kFlagCanReplace;
  if (p->kind == 1) {
    e.uniqueId = kSynthId;
    e.programCount = 3;
    e.paramCount = 2;
    e.inputCount = 0;
    e.outputCount = 4;
    e.flags |= abi::kFlagInstrument;
  } else {
    e.uniqueId = p->kind == 2 ? kFxId : 0x476C5453;  // 'GlTS'
    e.programCount = 1;
    e.paramCount = p->kind == 2 ? 1 : 0;
    e.inputCount = 2;
    e.outputCount = 2;
    if (p->kind == 2) {
      e.flags |= abi::kFlagChunks;
      p->params[0] = 0.5f;
    }
  }
  return &e;
}

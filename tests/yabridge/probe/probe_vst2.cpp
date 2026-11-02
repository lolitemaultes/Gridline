// Gridline yabridge test: the probe as a Windows VST2-style plugin, written against Gridline's own
// clean-room ABI description (src/host/vst2/Vst2Abi.h). Built twice: PROBE_SYNTH=1 is the
// instrument, PROBE_SYNTH=0 the effect. See probe_core.h.
#include "probe_core.h"
#include <new>
#include "host/vst2/Vst2Abi.h"

namespace abi = gl::vst2abi;

namespace {

struct Plugin final : probe::Host {
  abi::Effect fx{};
  abi::HostCallback hostCb;
  probe::Core core;
  probe::Window window;
  abi::Rect rect{0, 0, probe::kDefaultH, probe::kDefaultW};
  char chunk[probe::Core::kStateSize] = {};

  explicit Plugin(abi::HostCallback cb) : hostCb(cb), core(PROBE_SYNTH != 0), window(core, *this) {}
  void beginEdit(int param) override { hostCb(&fx, abi::hostBeginEdit, param, 0, nullptr, 0); }
  void performEdit(int param, double value) override { hostCb(&fx, abi::hostAutomate, param, 0, nullptr, static_cast<float>(value)); }
  void endEdit(int param) override { hostCb(&fx, abi::hostEndEdit, param, 0, nullptr, 0); }
  bool requestSize(int w, int h) override {
    rect.right = static_cast<int16_t>(w);
    rect.bottom = static_cast<int16_t>(h);
    const bool ok = hostCb(&fx, abi::hostResizeWindow, w, h, nullptr, 0) != 0;
    if (ok) window.resize(w, h);
    return ok;
  }
};

Plugin* self(abi::Effect* e) { return static_cast<Plugin*>(e->pluginObject); }
void copy(void* dst, const char* src) { std::strcpy(static_cast<char*>(dst), src); }

void processReplacing(abi::Effect* e, float** in, float** out, int32_t n) { self(e)->core.render(const_cast<const float* const*>(in), out, n); }
void setParam(abi::Effect* e, int32_t i, float v) {
  if (i < 0 || i >= probe::kParams) return;
  self(e)->core.p[i].store(static_cast<double>(v));
  probe::logf(self(e)->core.inst, "hostset %d %.6f", i, static_cast<double>(v));
}
float getParam(abi::Effect* e, int32_t i) { return i >= 0 && i < probe::kParams ? static_cast<float>(self(e)->core.p[i].load()) : 0.0f; }

intptr_t dispatch(abi::Effect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
  Plugin* p = self(e);
  switch (op) {
    case abi::opClose:
      p->window.close();
      probe::logf(p->core.inst, "destroy");
      delete p;
      return 1;
    case abi::opSetSampleRate:
      p->core.sr = static_cast<double>(opt);
      probe::logf(p->core.inst, "setup rate %.0f block 0 mode 0", static_cast<double>(opt));
      return 1;
    case abi::opPowerChanged:
      probe::logf(p->core.inst, "active %d", value ? 1 : 0);
      if (!value) p->core.logAudio();
      return 1;
    case abi::opGetCategory: return p->core.synth ? abi::catInstrument : abi::catEffect;
    case abi::opGetName:
      copy(ptr, p->core.synth ? "Gridline Probe Synth" : "Gridline Probe Fx");
      return 1;
    case abi::opGetVendor:
      copy(ptr, "Gridline Tests");
      return 1;
    case abi::opGetProduct:
      copy(ptr, "Gridline Probe");
      return 1;
    case abi::opGetVendorVersion: return 100;
    case abi::opGetApiVersion: return 2400;
    case abi::opGetParamName:
      if (index >= 0 && index < probe::kParams) copy(ptr, probe::kParamNames[index]);
      return 1;
    case abi::opGetParamUnit:
      copy(ptr, "");
      return 1;
    case abi::opGetParamText:
      if (index >= 0 && index < probe::kParams) std::sprintf(static_cast<char*>(ptr), "%.3f", p->core.p[index].load());
      return 1;
    case abi::opCanAutomate: return 1;
    case abi::opCanDo: {
      const char* s = static_cast<const char*>(ptr);
      if (p->core.synth && s && !std::strcmp(s, "receiveVstMidiEvent")) return 1;
      return -1;
    }
    case abi::opProcessEvents: {
      auto* list = static_cast<abi::EventList<2>*>(ptr);
      for (int32_t i = 0; i < list->count; ++i) {
        auto* m = static_cast<abi::MidiEvent*>(list->events[i]);
        if (m->type != 1) continue;
        const int st = m->data[0] & 0xF0;
        if (st == 0x90 && m->data[2] > 0) p->core.noteOn(m->data[1], m->data[2] / 127.0);
        else if (st == 0x80 || st == 0x90) p->core.noteOff(m->data[1]);
      }
      return 1;
    }
    case abi::opGetChunk:
      p->core.saveState(p->chunk);
      *static_cast<void**>(ptr) = p->chunk;
      return static_cast<intptr_t>(sizeof p->chunk);
    case abi::opSetChunk: return p->core.loadState(static_cast<const char*>(ptr), static_cast<size_t>(value)) ? 1 : 0;
    case abi::opEditorRect:
      *static_cast<abi::Rect**>(ptr) = &p->rect;
      return 1;
    case abi::opEditorOpen: return p->window.open(static_cast<HWND>(ptr), p->rect.right, p->rect.bottom) ? 1 : 0;
    case abi::opEditorClose: p->window.close(); return 1;
    default: return 0;
  }
}

}  // namespace

extern "C" __declspec(dllexport) abi::Effect* VSTPluginMain(abi::HostCallback host) {
  if (!host) return nullptr;
  auto* p = new (std::nothrow) Plugin(host);
  if (!p) return nullptr;
  abi::Effect& e = p->fx;
  e.magic = abi::kMagic;
  e.dispatch = dispatch;
  e.setParam = setParam;
  e.getParam = getParam;
  e.processReplacing = processReplacing;
  e.pluginObject = p;
  e.flags = abi::kFlagCanReplace | abi::kFlagChunks | abi::kFlagHasEditor | (p->core.synth ? abi::kFlagInstrument : 0);
  e.uniqueId = p->core.synth ? 0x476C5053 : 0x476C5046;  // 'GlPS', 'GlPF'
  e.programCount = 1;
  e.paramCount = probe::kParams;
  e.inputCount = p->core.synth ? 0 : 2;
  e.outputCount = 2;
  e.latency = p->core.synth ? 0 : probe::kLatency;
  return &e;
}

// Gridline — a strict CLAP instrument for the host tests (built in-tree as
// GridlineStrict.clap in its own folder; never installed). Written against the
// CLAP headers only. Like the strict VST3 synth (tests/vst3/StrictPlugin.cpp):
//   "org.gridline.strict.synth"  no audio input, three stereo output ports
//       (main on port 0), MIDI notes only;
//   "org.gridline.strict.mid"    the same with the main output on port 1,
//       between two extra outputs, CLAP and MIDI notes.
// A host that calls it out of order (process before start_processing,
// start_processing or process off the audio thread, deactivate while
// processing, too long blocks) or passes ports that do not match its layout
// gets silence and the probe says why. Silent for its first kWarmup frames
// (an emulation booting), 64 samples of reported latency.
#include <clap/clap.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../common/StrictProbe.h"

namespace {

GridlineStrictProbe gProbe;
constexpr uint32_t kPorts = 3;
constexpr int kLatency = 64;
constexpr int64_t kWarmup = 2048;

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const clap_plugin_descriptor_t kStrictDesc = {CLAP_VERSION_INIT, "org.gridline.strict.synth", "Gridline Strict Synth", "Gridline Tests", "", "", "",
                                              "1.0.0", "Strict host test instrument", kFeatures};
const clap_plugin_descriptor_t kMidDesc = {CLAP_VERSION_INIT, "org.gridline.strict.mid", "Gridline Strict Synth Mid", "Gridline Tests", "", "", "",
                                           "1.0.0", "Strict host test instrument, main output in the middle", kFeatures};

struct Strict {
  clap_plugin_t plugin;
  const clap_host_t* host = nullptr;
  const clap_host_thread_check_t* threads = nullptr;
  uint32_t main = 0;
  bool midiOnly = true;
  bool active = false, processing = false;
  double sr = 0;
  uint32_t maxFrames = 0;
  int64_t frames = 0, startAt = -1, stopAt = -1;
  double freq = 440, amp = 0, phase = 0;
};
Strict* of(const clap_plugin_t* p) { return static_cast<Strict*>(p->plugin_data); }
void setName(char* dst, const char* src) {
  std::strncpy(dst, src, CLAP_NAME_SIZE - 1);
  dst[CLAP_NAME_SIZE - 1] = 0;
}
bool offAudioThread(const Strict* s) { return s->threads && !s->threads->is_audio_thread(s->host); }

uint32_t portCount(const clap_plugin_t*, bool input) { return input ? 0 : kPorts; }
bool portGet(const clap_plugin_t* p, uint32_t i, bool input, clap_audio_port_info_t* info) {
  if (input || i >= kPorts) return false;
  const Strict* s = of(p);
  info->id = 100 + i;
  static const char* const kNames[2][kPorts] = {{"Main Out", "Out 3+4", "Out 5+6"}, {"Out 1+2", "Main Out", "Out 5+6"}};
  setName(info->name, kNames[s->main == 0 ? 0 : 1][i]);
  info->flags = i == s->main ? CLAP_AUDIO_PORT_IS_MAIN : 0;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kAudioPorts = {portCount, portGet};

uint32_t noteCount(const clap_plugin_t*, bool input) { return input ? 1 : 0; }
bool noteGet(const clap_plugin_t* p, uint32_t i, bool input, clap_note_port_info_t* info) {
  if (!input || i != 0) return false;
  info->id = 7;
  info->supported_dialects = of(p)->midiOnly ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = of(p)->midiOnly ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP;
  setName(info->name, "MIDI In");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {noteCount, noteGet};

uint32_t latency(const clap_plugin_t*) { return kLatency; }
const clap_plugin_latency_t kLatencyExt = {latency};

bool save(const clap_plugin_t*, const clap_ostream_t* st) {
  const int32_t magic = 0x53545231;
  return st->write(st, &magic, sizeof magic) == static_cast<int64_t>(sizeof magic);
}
bool load(const clap_plugin_t*, const clap_istream_t* st) {
  int32_t magic = 0;
  return st->read(st, &magic, sizeof magic) == static_cast<int64_t>(sizeof magic) && magic == 0x53545231;
}
const clap_plugin_state_t kState = {save, load};

const void* extension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatencyExt;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  return nullptr;
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
  Strict* s = of(p);
  bool ok = true;
  auto fail = [&](uint32_t bit) {
    gProbe.violations |= bit;
    ok = false;
  };
  if (!s->active) fail(kStrictProcessInactive);
  if (!s->processing) fail(kStrictProcessNotStarted);
  if (offAudioThread(s)) fail(kStrictWrongThread);
  if (pr->frames_count > s->maxFrames) fail(kStrictBlockTooLong);
  if (pr->audio_inputs_count != 0) fail(kStrictInputs);
  // CLAP: every port, with its channels.
  if (pr->audio_outputs_count != kPorts || !pr->audio_outputs) fail(kStrictLayout);
  else
    for (uint32_t b = 0; b < kPorts; ++b) {
      const clap_audio_buffer_t& o = pr->audio_outputs[b];
      if (o.channel_count != 2 || !o.data32 || !o.data32[0] || !o.data32[1]) fail(kStrictLayout);
    }
  gProbe.activeOutputs = static_cast<int32_t>(pr->audio_outputs_count);
  const int32_t n = static_cast<int32_t>(pr->frames_count);
  if (n <= 0) {
    ++gProbe.zeroBlocks;
    return CLAP_PROCESS_CONTINUE;
  }
  ++gProbe.blocks;
  gProbe.minBlock = gProbe.minBlock == 0 ? n : std::min(gProbe.minBlock, n);
  gProbe.maxBlock = std::max(gProbe.maxBlock, n);
  const clap_input_events_t* in = pr->in_events;
  const uint32_t evCount = in ? in->size(in) : 0;
  uint32_t ei = 0;
  for (int32_t i = 0; i < n; ++i) {
    for (; ei < evCount; ++ei) {
      const clap_event_header_t* h = in->get(in, ei);
      if (!h || static_cast<int32_t>(h->time) > i) break;
      if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
      int key = -1;
      bool on = false;
      if (h->type == CLAP_EVENT_MIDI) {
        const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
        const int st = m->data[0] & 0xF0;
        if (st == 0x90 && m->data[2] > 0) key = m->data[1], on = true;
        else if (st == 0x80 || st == 0x90) key = m->data[1];
      } else if (!s->midiOnly && (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)) {
        const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
        key = e->key;
        on = h->type == CLAP_EVENT_NOTE_ON;
      } else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
        gProbe.violations |= kStrictEventsInactive;  // a dialect the port does not take
      }
      if (key < 0) continue;
      if (on) {
        s->freq = 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
        s->startAt = s->frames + kLatency;
        s->stopAt = -1;
        ++gProbe.notesOn;
      } else {
        s->stopAt = s->frames + kLatency;
      }
    }
    const bool ready = ok && s->frames >= kWarmup;
    if (s->startAt >= 0 && s->frames >= s->startAt) s->amp = 0.5, s->startAt = -1, s->phase = 0;
    if (s->stopAt >= 0 && s->frames >= s->stopAt) s->amp = 0, s->stopAt = -1;
    float x = 0;
    if (ready && s->amp > 0) {
      s->phase += s->freq / s->sr;
      s->phase -= std::floor(s->phase);
      x = static_cast<float>(std::sin(6.283185307179586 * s->phase) * s->amp);
    }
    if (pr->audio_outputs)
      for (uint32_t b = 0; b < kPorts && b < pr->audio_outputs_count; ++b) {
        const clap_audio_buffer_t& o = pr->audio_outputs[b];
        if (o.channel_count < 2 || !o.data32 || !o.data32[0] || !o.data32[1]) continue;
        const uint32_t k = b == s->main ? 0 : b < s->main ? b + 1 : b;
        o.data32[0][i] = o.data32[1][i] = x / static_cast<float>(1u << k);
      }
    ++s->frames;
  }
  gProbe.frames = s->frames;
  return CLAP_PROCESS_CONTINUE;
}

const clap_plugin_t* make(const clap_host_t* host, const clap_plugin_descriptor_t* desc, uint32_t main, bool midiOnly) {
  Strict* s = new Strict;
  s->host = host;
  s->main = main;
  s->midiOnly = midiOnly;
  gProbe.mainBus = static_cast<int32_t>(main);
  clap_plugin_t& p = s->plugin;
  p.desc = desc;
  p.plugin_data = s;
  p.init = [](const clap_plugin_t* pl) {
    Strict* st = of(pl);
    st->threads = static_cast<const clap_host_thread_check_t*>(st->host->get_extension(st->host, CLAP_EXT_THREAD_CHECK));
    return true;
  };
  p.destroy = [](const clap_plugin_t* pl) { delete of(pl); };
  p.activate = [](const clap_plugin_t* pl, double sr, uint32_t, uint32_t maxFrames) {
    Strict* st = of(pl);
    if (st->active) gProbe.violations |= kStrictSetupWhileActive;
    if (sr <= 0 || maxFrames == 0) {
      gProbe.violations |= kStrictActiveWithoutSetup;
      return false;
    }
    st->sr = sr;
    st->maxFrames = maxFrames;
    st->active = true;
    st->frames = 0, st->amp = 0, st->startAt = st->stopAt = -1;
    gProbe.sampleRate = sr;
    gProbe.setupMaxBlock = static_cast<int32_t>(maxFrames);
    return true;
  };
  p.deactivate = [](const clap_plugin_t* pl) {
    Strict* st = of(pl);
    if (st->processing) gProbe.violations |= kStrictDeactivateProcessing;
    st->active = false;
  };
  p.start_processing = [](const clap_plugin_t* pl) {
    Strict* st = of(pl);
    if (!st->active) gProbe.violations |= kStrictProcessInactive;
    if (offAudioThread(st)) gProbe.violations |= kStrictWrongThread;
    st->processing = true;
    ++gProbe.starts;
    return true;
  };
  p.stop_processing = [](const clap_plugin_t* pl) {
    Strict* st = of(pl);
    if (offAudioThread(st)) gProbe.violations |= kStrictWrongThread;
    st->processing = false;
  };
  p.reset = [](const clap_plugin_t* pl) { of(pl)->amp = 0; };
  p.process = process;
  p.get_extension = extension;
  p.on_main_thread = [](const clap_plugin_t*) {};
  return &s->plugin;
}

uint32_t count(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kStrictDesc : i == 1 ? &kMidDesc : nullptr; }
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!std::strcmp(id, kStrictDesc.id)) return make(host, &kStrictDesc, 0, true);
  if (!std::strcmp(id, kMidDesc.id)) return make(host, &kMidDesc, 1, false);
  return nullptr;
}
const clap_plugin_factory_t kFactory = {count, descriptor, create};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
extern "C" CLAP_EXPORT GridlineStrictProbe* gridline_strict_probe() { return &gProbe; }

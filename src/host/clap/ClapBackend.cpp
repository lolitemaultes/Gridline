// Gridline — CLAP hosting (format "clap"): module loading, scanning and the
// host::Backend over one clap_plugin. Uses the CLAP headers (MIT), vendored
// unmodified in third_party/clap.
//
// Thread model: create / activate / state / params info / gui / timers /
// on_main_thread on the UI ("main") thread; start_processing / process on the
// audio thread. Parameter edits made in the plugin's editor arrive as output
// events of process() (or of params.flush while inactive) and are queued for
// the UI thread without locks.
//
// Parameter values: the host keeps an atomic cache of every parameter's
// plain value, written by the audio thread from the events it sends and
// receives. While the plugin is active the main thread reads the cache
// instead of calling params.get_value concurrently with process() (plugins
// are not required to make that pair race-free in practice); get_value runs
// while inactive and right after main-thread changes (state, presets,
// rescans) to refresh the cache.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "../Dylib.h"
#include "../PluginScan.h"
#include "clap/clap.h"
#include "../../core/Platform.h"

namespace gl::host {

namespace {

// ---------------------------------------------------------------- module

struct ClapModule {
  std::shared_ptr<Dylib> lib;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_factory_t* factory = nullptr;
};

std::shared_ptr<ClapModule> loadModule(const std::string& path, std::string* err) {
  static std::mutex lock;
  static auto* cache = new std::map<std::string, std::shared_ptr<ClapModule>>;  // never destroyed
  std::lock_guard<std::mutex> g(lock);
  if (auto it = cache->find(path); it != cache->end()) return it->second;
  auto lib = Dylib::open(path, err);
  if (!lib) return nullptr;
  auto m = std::make_shared<ClapModule>();
  m->lib = lib;
  m->entry = static_cast<const clap_plugin_entry_t*>(lib->data("clap_entry"));
  if (!m->entry) {
    if (err) *err = "no clap_entry export";
    return nullptr;
  }
  if (!clap_version_is_compatible(m->entry->clap_version)) {
    if (err) *err = "incompatible CLAP version";
    return nullptr;
  }
  if (!m->entry->init(path.c_str())) {
    if (err) *err = "clap_entry.init failed";
    return nullptr;
  }
  m->factory = static_cast<const clap_plugin_factory_t*>(m->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!m->factory) {
    if (err) *err = "no plugin factory";
    return nullptr;
  }
  (*cache)[path] = m;
  return m;
}

thread_local bool tlsAudio = false;  // this thread acts as the audio thread (see thread-check)

// ---------------------------------------------------------------- event lists

// Input events of one block: fixed capacity, kept sorted by time (stable).
class InEvents {
 public:
  static constexpr uint32_t kMax = 2048;
  union Ev {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_note_expression_t expr;
    clap_event_param_value_t param;
    clap_event_param_mod_t mod;
    clap_event_midi_t midi;
  };
  InEvents() : evs_(new Ev[kMax]) {
    list_.ctx = this;
    list_.size = [](const clap_input_events_t* l) { return static_cast<InEvents*>(l->ctx)->count_; };
    list_.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
      auto* self = static_cast<InEvents*>(l->ctx);
      return i < self->count_ ? &self->evs_[i].header : nullptr;
    };
  }
  bool add(const Ev& e) {
    if (count_ >= kMax) return false;
    uint32_t i = count_++;
    while (i > 0 && evs_[i - 1].header.time > e.header.time) {
      evs_[i] = evs_[i - 1];
      --i;
    }
    evs_[i] = e;
    return true;
  }
  void clear() { count_ = 0; }
  uint32_t size() const { return count_; }
  const Ev& at(uint32_t i) const { return evs_[i]; }
  const clap_input_events_t* list() const { return &list_; }

 private:
  std::unique_ptr<Ev[]> evs_;
  uint32_t count_ = 0;
  clap_input_events_t list_{};
};

clap_event_header_t header(uint16_t type, uint32_t size, int offset) {
  clap_event_header_t h{};
  h.size = size;
  h.time = static_cast<uint32_t>(std::max(0, offset));
  h.space_id = CLAP_CORE_EVENT_SPACE_ID;
  h.type = type;
  h.flags = 0;
  return h;
}

// ---------------------------------------------------------------- backend

class ClapBackend;

class ClapGui final : public Editor {
 public:
  ClapGui(ClapBackend* b) : b_(b) {}
  ~ClapGui() override;
  bool size(int& w, int& h) override;
  bool canResize() const override;
  void constrain(int& w, int& h) override;
  bool attach(void* parent) override;
  void setSize(int w, int h) override;
  void setContentScale(double scale) override;
  bool created = false;

 private:
  ClapBackend* b_;
};

class ClapBackend final : public Backend {
 public:
  static std::unique_ptr<ClapBackend> create(const std::string& path, const std::string& id, std::string* err) {
    auto mod = loadModule(path, err);
    if (!mod) return nullptr;
    std::unique_ptr<ClapBackend> b(new ClapBackend);
    b->mod_ = mod;
    b->mainThread_ = std::this_thread::get_id();
    b->plugin_ = mod->factory->create_plugin(mod->factory, &b->host_, id.c_str());
    if (!b->plugin_) {
      if (err) *err = "cannot create the plugin " + id;
      return nullptr;
    }
    if (!b->plugin_->init(b->plugin_)) {
      b->plugin_->destroy(b->plugin_);
      b->plugin_ = nullptr;
      if (err) *err = "plugin init failed";
      return nullptr;
    }
    b->desc_ = b->plugin_->desc;
    auto ext = [&](const char* id2) { return b->plugin_->get_extension(b->plugin_, id2); };
    b->params_ = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
    b->state_ = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
    b->latencyExt_ = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
    b->tailExt_ = static_cast<const clap_plugin_tail_t*>(ext(CLAP_EXT_TAIL));
    b->gui_ = static_cast<const clap_plugin_gui_t*>(ext(CLAP_EXT_GUI));
    b->audioPorts_ = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
    b->notePorts_ = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS));
    b->timerExt_ = static_cast<const clap_plugin_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
    b->render_ = static_cast<const clap_plugin_render_t*>(ext(CLAP_EXT_RENDER));
    b->presetLoad_ = static_cast<const clap_plugin_preset_load_t*>(ext(CLAP_EXT_PRESET_LOAD));
    if (!b->presetLoad_) b->presetLoad_ = static_cast<const clap_plugin_preset_load_t*>(ext(CLAP_EXT_PRESET_LOAD_COMPAT));
    b->readParams();
    b->readPorts();
    return b;
  }

  ~ClapBackend() override {
    if (!plugin_) return;
    editorGone();
    deactivate();
    plugin_->destroy(plugin_);
  }

  const clap_plugin_t* plugin() const { return plugin_; }
  const clap_plugin_descriptor_t* descriptor() const { return desc_; }

  // ---- description ----
  const char* format() const override { return "CLAP"; }
  const std::vector<ParamInfo>& params() const override { return params2_; }
  bool hasEventInput() const override { return noteIn_; }
  int mainInputChannels() const override { return mainIn_ >= 0 ? inCh_[static_cast<size_t>(mainIn_)] : 0; }
  int mainOutputChannels() const override { return mainOut_ >= 0 ? outCh_[static_cast<size_t>(mainOut_)] : 0; }
  int sidechainChannels() const override { return scIn_ >= 0 ? inCh_[static_cast<size_t>(scIn_)] : 0; }
  int auxOutputCount() const override { return static_cast<int>(auxOut_.size()); }
  std::string auxOutputName(int k) const override {
    return k >= 0 && static_cast<size_t>(k) < auxOut_.size() && !outNames_[static_cast<size_t>(auxOut_[static_cast<size_t>(k)])].empty()
               ? outNames_[static_cast<size_t>(auxOut_[static_cast<size_t>(k)])]
               : Backend::auxOutputName(k);
  }
  bool supportsTuning() const override { return clapDialect_; }
  bool hasEventOutput() const override { return noteOut_; }
  bool supportsParamMod(uint32_t id) const override {
    const Range* r = range(id);
    return r && r->modulatable;
  }
  bool hasBypass() const override { return bypassId_ >= 0; }

  // ---- UI thread ----
  bool prepare(double sr, int maxBlock) override {
    if (active_ && sr == sampleRate_ && maxBlock == maxBlock_) return true;
    deactivate();
    sampleRate_ = sr;
    maxBlock_ = std::max(1, maxBlock);
    readPorts();
    allocate();
    refreshCache();  // last main-thread read before the audio thread may run
    if (!plugin_->activate(plugin_, sampleRate_, 1, static_cast<uint32_t>(maxBlock_))) return false;
    active_ = true;
    latency_.store(latencyExt_ ? static_cast<int>(latencyExt_->get(plugin_)) : 0);
    return true;
  }
  bool setOffline(bool offline) override {
    if (!render_) return false;
    return render_->set(plugin_, offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
  }
  bool setBypass(bool on) override {
    if (bypassId_ < 0) return false;
    pendingBypass_.store(on ? 1 : 0);
    if (!active_ && params_) {  // not processing: through params.flush
      InEvents::Ev e{};
      const Range* r = range(static_cast<uint32_t>(bypassId_));
      e.param.header = header(CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value_t), 0);
      e.param.param_id = static_cast<clap_id>(bypassId_);
      e.param.cookie = r ? r->cookie : nullptr;
      e.param.note_id = -1;
      e.param.port_index = e.param.channel = e.param.key = -1;
      e.param.value = on ? (r ? r->max : 1.0) : (r ? r->min : 0.0);
      InEvents one;
      one.add(e);
      params_->flush(plugin_, one.list(), &outEvents_);
      pendingBypass_.store(-1);
    }
    return true;
  }
  std::vector<PresetRef> discoverPresets() override;
  bool loadPreset(const PresetRef& p) override {
    if (!presetLoad_) return false;
    const bool ok = presetLoad_->from_location(plugin_, p.locationKind, p.location.empty() ? nullptr : p.location.c_str(),
                                               p.key.empty() ? nullptr : p.key.c_str());
    refreshCache();
    return ok;
  }
  double sampleRate() const override { return sampleRate_; }
  int maxBlock() const override { return maxBlock_; }
  bool active() const override { return active_; }
  void restart(int32_t flags) override {
    if (flags & kRestartReload) {
      double sr = sampleRate_;
      int mb = maxBlock_;
      deactivate();
      if (sr > 0) prepare(sr, mb);
    } else if ((flags & kRestartLatency) && latencyExt_) {
      if (active_) {
        // Latency may only be read while active; the change applies on reactivation.
        double sr = sampleRate_;
        int mb = maxBlock_;
        deactivate();
        prepare(sr, mb);
      }
    }
    if (flags & kRestartParamInfo) readParams();
  }
  int latency() const override { return latency_.load(); }
  int tailSamples() const override {
    uint32_t t = tailExt_ && active_ ? tailExt_->get(plugin_) : 0;
    return static_cast<int>(std::min<uint32_t>(t, 1u << 30));
  }
  bool getState(std::string& data) override {
    if (!state_) return false;
    std::string bytes;
    clap_ostream_t os{};
    os.ctx = &bytes;
    os.write = [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
      static_cast<std::string*>(s->ctx)->append(static_cast<const char*>(buf), static_cast<size_t>(size));
      return static_cast<int64_t>(size);
    };
    if (!state_->save(plugin_, &os)) return false;
    data = "clap1:" + base64Encode(bytes);
    return true;
  }
  bool setState(const std::string& data) override {
    if (!state_ || data.compare(0, 6, "clap1:") != 0) return false;
    std::string bytes;
    if (!base64Decode(data.substr(6), bytes)) return false;
    struct Reader {
      const std::string* s;
      size_t pos;
    } rd{&bytes, 0};
    clap_istream_t is{};
    is.ctx = &rd;
    is.read = [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
      auto* r = static_cast<Reader*>(s->ctx);
      size_t n = static_cast<size_t>(std::min<uint64_t>(size, r->s->size() - r->pos));
      std::memcpy(buf, r->s->data() + r->pos, n);
      r->pos += n;
      return static_cast<int64_t>(n);
    };
    const bool ok = state_->load(plugin_, &is);
    refreshCache();
    return ok;
  }
  // Main thread. While active: the cache (see the file comment).
  double paramNormalized(uint32_t id) const override {
    const Range* r = range(id);
    if (!r) return 0.0;
    auto* self = const_cast<ClapBackend*>(this);
    if (cacheStale_.exchange(false)) self->refreshCache();
    if (active_) return r->toNorm(cache_[static_cast<size_t>(r - ranges_.data())].load(std::memory_order_relaxed));
    double v = 0;
    if (!params_ || !params_->get_value(plugin_, id, &v)) return r->defNorm;
    cache_[static_cast<size_t>(r - ranges_.data())].store(v, std::memory_order_relaxed);
    return r->toNorm(v);
  }
  std::string paramText(uint32_t id, double n) const override {
    const Range* r = range(id);
    if (!r || !params_ || !params_->value_to_text) return {};
    char buf[256] = {};
    if (!params_->value_to_text(plugin_, id, r->toPlain(n), buf, sizeof buf)) return {};
    return std::string(buf, strnlen(buf, sizeof buf));
  }
  void drainHostEvents(std::vector<HostEvent>& out) override {
    HostEvent e;
    while (events_.pop(e)) out.push_back(e);
    int32_t f = restartFlags_.exchange(0);
    if (f) {
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
  bool hasEditor() const override {
    return gui_ && gui_->is_api_supported(plugin_, windowApi(), false);
  }
  std::unique_ptr<Editor> createEditor() override {
    if (!hasEditor() || editor_) return nullptr;
    if (!gui_->create(plugin_, windowApi(), false)) return nullptr;
    auto e = std::make_unique<ClapGui>(this);
    e->created = true;
    editor_ = e.get();
    return e;
  }
  void idle() override {
    if (callback_.exchange(false)) plugin_->on_main_thread(plugin_);
    // Parameter changes made while not processing (inactive): flush on the main thread.
    if (flush_.exchange(false) && params_ && !active_) {
      InEvents none;
      params_->flush(plugin_, none.list(), &outEvents_);
    }
    if (!timers_.empty() && timerExt_) {
      const double now = seconds();
      for (size_t i = 0; i < timers_.size(); ++i) {
        if (now < timers_[i].due) continue;
        timers_[i].due = now + timers_[i].period;
        timerExt_->on_timer(plugin_, timers_[i].id);
      }
    }
  }

  // ---- audio thread ----
  float* inputChannel(int ch) override { return port(inPtr_, inCh_, mainIn_, ch); }
  float* sidechainChannel(int ch) override { return port(inPtr_, inCh_, scIn_, ch); }
  int outputChannels(int bus) const override {
    int p = bus == 0 ? mainOut_ : (bus > 0 && static_cast<size_t>(bus) <= auxOut_.size() ? auxOut_[static_cast<size_t>(bus - 1)] : -1);
    return p >= 0 ? outCh_[static_cast<size_t>(p)] : 0;
  }
  const float* outputChannel(int bus, int ch) override {
    int p = bus == 0 ? mainOut_ : (bus > 0 && static_cast<size_t>(bus) <= auxOut_.size() ? auxOut_[static_cast<size_t>(bus - 1)] : -1);
    return port(outPtr_, outCh_, p, ch);
  }
  bool addParamChange(uint32_t id, double v, int offset) override {
    const Range* r = range(id);
    if (!r) return true;  // unknown: drop
    InEvents::Ev e{};
    e.param.header = header(CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value_t), offset);
    e.param.param_id = id;
    e.param.cookie = r->cookie;
    e.param.note_id = -1;
    e.param.port_index = -1;
    e.param.channel = -1;
    e.param.key = -1;
    e.param.value = r->toPlain(v);
    return in_.add(e);
  }
  bool addParamMod(uint32_t id, double amount, int offset) override {
    const Range* r = range(id);
    if (!r || !r->modulatable) return false;
    InEvents::Ev e{};
    e.mod.header = header(CLAP_EVENT_PARAM_MOD, sizeof(clap_event_param_mod_t), offset);
    e.mod.param_id = id;
    e.mod.cookie = r->cookie;
    e.mod.note_id = -1;
    e.mod.port_index = -1;
    e.mod.channel = -1;
    e.mod.key = -1;
    e.mod.amount = amount * (r->max - r->min);  // plain units
    return in_.add(e);
  }
  bool popMidiOut(MidiOut& m) override { return midiOut_.pop(m); }
  void addEvent(const NoteEvent& n) override {
    InEvents::Ev e{};
    switch (n.type) {
      case NoteEvent::NoteOn:
      case NoteEvent::NoteOff:
        if (!clapDialect_ && midiDialect_) {  // MIDI-only instruments
          e.midi.header = header(CLAP_EVENT_MIDI, sizeof(clap_event_midi_t), n.offset);
          e.midi.port_index = static_cast<uint16_t>(notePort_);
          e.midi.data[0] = static_cast<uint8_t>((n.type == NoteEvent::NoteOn ? 0x90 : 0x80) | (n.channel & 0x0F));
          e.midi.data[1] = static_cast<uint8_t>(n.key & 0x7F);
          e.midi.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127)), n.type == NoteEvent::NoteOn ? 1 : 0, 127));
          in_.add(e);
          return;
        }
        e.note.header = header(n.type == NoteEvent::NoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t), n.offset);
        e.note.note_id = n.noteId;
        e.note.port_index = static_cast<int16_t>(notePort_);
        e.note.channel = n.channel;
        e.note.key = n.key;
        e.note.velocity = std::clamp(static_cast<double>(n.velocity), 0.0, 1.0);
        in_.add(e);
        if (n.type == NoteEvent::NoteOn && n.tuning != 0 && clapDialect_) {
          InEvents::Ev t{};
          t.expr.header = header(CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t), n.offset);
          t.expr.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
          t.expr.note_id = n.noteId;
          t.expr.port_index = static_cast<int16_t>(notePort_);
          t.expr.channel = n.channel;
          t.expr.key = n.key;
          t.expr.value = n.tuning;
          in_.add(t);
        }
        if (n.type == NoteEvent::NoteOn && n.hasProps && n.modX != 0.5f && clapDialect_) {  // note property X → brightness
          InEvents::Ev t{};
          t.expr.header = header(CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t), n.offset);
          t.expr.expression_id = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
          t.expr.note_id = n.noteId;
          t.expr.port_index = static_cast<int16_t>(notePort_);
          t.expr.channel = n.channel;
          t.expr.key = n.key;
          t.expr.value = std::clamp(static_cast<double>(n.modX), 0.0, 1.0);
          in_.add(t);
        }
        return;
      case NoteEvent::Tuning:
        if (!clapDialect_) return;
        e.expr.header = header(CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t), n.offset);
        e.expr.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
        e.expr.note_id = n.noteId;
        e.expr.port_index = static_cast<int16_t>(notePort_);
        e.expr.channel = n.channel;
        e.expr.key = n.key;
        e.expr.value = std::clamp(n.tuning, -120.0, 120.0);
        in_.add(e);
        return;
      case NoteEvent::Midi:
        if (!midiDialect_ && !noteIn_) return;
        e.midi.header = header(CLAP_EVENT_MIDI, sizeof(clap_event_midi_t), n.offset);
        e.midi.port_index = static_cast<uint16_t>(notePort_);
        std::memcpy(e.midi.data, n.midi, 3);
        in_.add(e);
        return;
    }
  }
  void clearInput() override { in_.clear(); }
  bool process(int n, const Transport& t) override {
    if (!active_ || n <= 0 || n > maxBlock_) {
      for (size_t p = 0; p < outPtr_.size(); ++p)
        for (int c = 0; c < outCh_[p]; ++c) std::memset(outPtr_[p][static_cast<size_t>(c)], 0, sizeof(float) * static_cast<size_t>(std::max(0, std::min(n, maxBlock_))));
      in_.clear();
      return false;
    }
    tlsAudio = true;
    if (!processing_) processing_ = plugin_->start_processing(plugin_);
    if (const int bp = pendingBypass_.exchange(-1); bp >= 0 && bypassId_ >= 0) {
      const Range* r = range(static_cast<uint32_t>(bypassId_));
      InEvents::Ev e{};
      e.param.header = header(CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value_t), 0);
      e.param.param_id = static_cast<clap_id>(bypassId_);
      e.param.cookie = r ? r->cookie : nullptr;
      e.param.note_id = -1;
      e.param.port_index = e.param.channel = e.param.key = -1;
      e.param.value = bp ? (r ? r->max : 1.0) : (r ? r->min : 0.0);
      in_.add(e);
    }
    const double bpm = std::max(1.0, t.tempo);
    auto beats = [](double qn) { return static_cast<clap_beattime>(std::llround(qn * static_cast<double>(CLAP_BEATTIME_FACTOR))); };
    auto secs = [bpm](double qn) { return static_cast<clap_sectime>(std::llround(qn * 60.0 / bpm * static_cast<double>(CLAP_SECTIME_FACTOR))); };
    clap_event_transport_t tr{};
    tr.header = header(CLAP_EVENT_TRANSPORT, sizeof tr, 0);
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
               (t.playing ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u) | (t.recording ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_RECORDING) : 0u) |
               (t.looping ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_LOOP_ACTIVE) : 0u);
    tr.song_pos_beats = beats(t.projectQn);
    tr.song_pos_seconds = secs(t.projectQn);
    tr.tempo = t.tempo;
    tr.tempo_inc = 0;
    tr.loop_start_beats = beats(t.loopStartQn);
    tr.loop_end_beats = beats(t.loopEndQn);
    tr.loop_start_seconds = secs(t.loopStartQn);
    tr.loop_end_seconds = secs(t.loopEndQn);
    tr.bar_start = beats(t.barQn);
    tr.bar_number = t.barNumber;
    tr.tsig_num = static_cast<uint16_t>(std::clamp(t.tsNum, 1, 64));
    tr.tsig_denom = static_cast<uint16_t>(std::clamp(t.tsDen, 1, 64));
    clap_process_t pr{};
    pr.steady_time = t.continuousSamples;
    pr.frames_count = static_cast<uint32_t>(n);
    pr.transport = &tr;
    pr.audio_inputs = inBuf_.empty() ? nullptr : inBuf_.data();
    pr.audio_inputs_count = static_cast<uint32_t>(inBuf_.size());
    pr.audio_outputs = outBuf_.empty() ? nullptr : outBuf_.data();
    pr.audio_outputs_count = static_cast<uint32_t>(outBuf_.size());
    pr.in_events = in_.list();
    pr.out_events = &outEvents_;
    for (auto& b : inBuf_) b.constant_mask = 0;
    for (auto& b : outBuf_) b.constant_mask = 0;
    midiOut_.clear();
    inProcess_ = true;
    clap_process_status st = plugin_->process(plugin_, &pr);
    inProcess_ = false;
    // The values the plugin now has (the cache the main thread reads).
    for (uint32_t i = 0; i < in_.size(); ++i) {
      const InEvents::Ev& e = in_.at(i);
      if (e.header.type == CLAP_EVENT_PARAM_VALUE && e.header.space_id == CLAP_CORE_EVENT_SPACE_ID) cacheStore(e.param.param_id, e.param.value);
    }
    in_.clear();
    tlsAudio = false;
    if (st == CLAP_PROCESS_ERROR) {
      for (size_t p = 0; p < outPtr_.size(); ++p)
        for (int c = 0; c < outCh_[p]; ++c) std::memset(outPtr_[p][static_cast<size_t>(c)], 0, sizeof(float) * static_cast<size_t>(n));
    }
    return true;
  }

  // ---- host callbacks ----
  static ClapBackend* self(const clap_host_t* h) { return static_cast<ClapBackend*>(h->host_data); }
  void editorGone() {
    if (editor_ && editor_->created && gui_) gui_->destroy(plugin_);
    if (editor_) editor_->created = false;
    editor_ = nullptr;
  }
  const clap_plugin_gui_t* gui() const { return gui_; }
  static const char* windowApi() {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
  }
  ClapGui* editor_ = nullptr;

 private:
  struct Range {
    uint32_t id = 0;
    double min = 0, max = 1, defNorm = 0;
    void* cookie = nullptr;
    bool modulatable = false;
    double toNorm(double v) const { return max > min ? std::clamp((v - min) / (max - min), 0.0, 1.0) : 0.0; }
    double toPlain(double n) const { return min + std::clamp(n, 0.0, 1.0) * (max - min); }
  };
  struct Timer {
    clap_id id;
    double period, due;
  };

  ClapBackend() {
    host_.clap_version = CLAP_VERSION;
    host_.host_data = this;
    host_.name = "Gridline";
    host_.vendor = "Gridline";
    host_.url = "";
    host_.version = "0.5";
    host_.get_extension = &ClapBackend::hostExtension;
    host_.request_restart = [](const clap_host_t* h) { self(h)->restartFlags_ |= kRestartReload; };
    host_.request_process = [](const clap_host_t*) {};
    host_.request_callback = [](const clap_host_t* h) { self(h)->callback_.store(true); };
    outEvents_.ctx = this;
    outEvents_.try_push = [](const clap_output_events_t* l, const clap_event_header_t* e) {
      static_cast<ClapBackend*>(l->ctx)->outputEvent(e);
      return true;
    };
  }

  static double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // Edits from the plugin (its editor): param value / gesture output events.
  void outputEvent(const clap_event_header_t* e) {
    if (!e || e->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    HostEvent h;
    if (e->type == CLAP_EVENT_NOTE_ON || e->type == CLAP_EVENT_NOTE_OFF || e->type == CLAP_EVENT_MIDI) {
      if (!inProcess_) return;  // notes only come out of process()
      if (e->type == CLAP_EVENT_MIDI) {
        auto* m = reinterpret_cast<const clap_event_midi_t*>(e);
        midiOut_.push(static_cast<int>(e->time), m->data[0], m->data[1], m->data[2]);
      } else {
        auto* n = reinterpret_cast<const clap_event_note_t*>(e);
        const uint8_t ch = static_cast<uint8_t>(std::clamp<int>(n->channel, 0, 15));
        const uint8_t key = static_cast<uint8_t>(std::clamp<int>(n->key, 0, 127));
        const int vel = static_cast<int>(std::lround(std::clamp(n->velocity, 0.0, 1.0) * 127));
        if (e->type == CLAP_EVENT_NOTE_ON)
          midiOut_.push(static_cast<int>(e->time), static_cast<uint8_t>(0x90 | ch), key, static_cast<uint8_t>(std::max(1, vel)));
        else
          midiOut_.push(static_cast<int>(e->time), static_cast<uint8_t>(0x80 | ch), key, static_cast<uint8_t>(vel));
      }
      return;
    }
    if (e->type == CLAP_EVENT_PARAM_VALUE) {
      auto* p = reinterpret_cast<const clap_event_param_value_t*>(e);
      const Range* r = range(p->param_id);
      if (!r) return;
      cacheStore(p->param_id, p->value);
      h.kind = HostEvent::PerformEdit;
      h.id = p->param_id;
      h.value = r->toNorm(p->value);
    } else if (e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || e->type == CLAP_EVENT_PARAM_GESTURE_END) {
      auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(e);
      h.kind = e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? HostEvent::BeginEdit : HostEvent::EndEdit;
      h.id = g->param_id;
    } else {
      return;
    }
    events_.push(h);
  }

  static const void* hostExtension(const clap_host_t* h, const char* id) {
    (void)h;
    static const clap_host_params_t params = {
        [](const clap_host_t* hh, clap_param_rescan_flags f) {
          if (f & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) self(hh)->restartFlags_ |= kRestartParamInfo;
          if (f & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) {
            self(hh)->restartFlags_ |= kRestartParamValues;
            self(hh)->cacheStale_.store(true);
          }
        },
        [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
        [](const clap_host_t* hh) { self(hh)->flush_.store(true); },
    };
    static const clap_host_state_t state = {[](const clap_host_t* hh) { self(hh)->dirty_.store(true); }};
    static const clap_host_latency_t latency = {[](const clap_host_t* hh) { self(hh)->restartFlags_ |= kRestartLatency; }};
    static const clap_host_tail_t tail = {[](const clap_host_t*) {}};
    static const clap_host_log_t log = {[](const clap_host_t*, clap_log_severity sev, const char* msg) {
      if (sev >= CLAP_LOG_ERROR && msg) std::fprintf(stderr, "[clap] %s\n", msg);
    }};
    static const clap_host_thread_check_t threads = {
        [](const clap_host_t* hh) { return std::this_thread::get_id() == self(hh)->mainThread_ && !tlsAudio; },
        [](const clap_host_t*) { return tlsAudio; },
    };
    static const clap_host_audio_ports_t ports = {
        [](const clap_host_t*, uint32_t) { return true; },
        [](const clap_host_t* hh, uint32_t) { self(hh)->restartFlags_ |= kRestartReload; },
    };
    static const clap_host_note_ports_t notes = {
        [](const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
        [](const clap_host_t* hh, uint32_t) { self(hh)->restartFlags_ |= kRestartReload; },
    };
    static const clap_host_gui_t gui = {
        [](const clap_host_t*) {},
        [](const clap_host_t* hh, uint32_t w, uint32_t hgt) {
          ClapBackend* b = self(hh);
          return b->editor_ && b->editor_->requestResize && b->editor_->requestResize(static_cast<int>(w), static_cast<int>(hgt));
        },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t* hh, bool destroyed) {
          if (destroyed && self(hh)->editor_) self(hh)->editor_->created = false;
        },
    };
    static const clap_host_timer_support_t timer = {
        [](const clap_host_t* hh, uint32_t ms, clap_id* tid) {
          ClapBackend* b = self(hh);
          *tid = b->nextTimer_++;
          double p = std::max<uint32_t>(ms, 5) / 1000.0;
          b->timers_.push_back({*tid, p, seconds() + p});
          return true;
        },
        [](const clap_host_t* hh, clap_id tid) {
          auto& t = self(hh)->timers_;
          t.erase(std::remove_if(t.begin(), t.end(), [&](const Timer& x) { return x.id == tid; }), t.end());
          return true;
        },
    };
    static const clap_host_preset_load_t presetLoad = {
        [](const clap_host_t*, uint32_t, const char*, const char*, int32_t, const char* msg) {
          if (msg) std::fprintf(stderr, "[clap] preset: %s\n", msg);
        },
        [](const clap_host_t* hh, uint32_t, const char*, const char*) {
          self(hh)->restartFlags_ |= kRestartParamValues;
          self(hh)->cacheStale_.store(true);
          self(hh)->dirty_.store(true);
        },
    };
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &presetLoad;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &threads;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timer;
    return nullptr;
  }

  void deactivate() {
    if (!active_) return;
    if (processing_) {
      tlsAudio = true;  // the audio thread is suspended: act as it
      plugin_->stop_processing(plugin_);
      tlsAudio = false;
      processing_ = false;
    }
    plugin_->deactivate(plugin_);
    active_ = false;
  }

  // Every parameter's value into the cache (main thread).
  void refreshCache() {
    if (!params_) return;
    for (size_t i = 0; i < ranges_.size(); ++i) {
      double v = 0;
      if (params_->get_value(plugin_, ranges_[i].id, &v)) cache_[i].store(v, std::memory_order_relaxed);
    }
  }
  void cacheStore(clap_id id, double plain) {
    const Range* r = range(id);
    if (r) cache_[static_cast<size_t>(r - ranges_.data())].store(plain, std::memory_order_relaxed);
  }

  void readParams() {
    params2_.clear();
    ranges_.clear();
    bypassId_ = -1;
    if (params_) {
      const uint32_t n = params_->count(plugin_);
      for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t pi{};
        if (!params_->get_info(plugin_, i, &pi)) continue;
        Range r;
        r.id = pi.id;
        r.min = pi.min_value;
        r.max = pi.max_value;
        r.cookie = pi.cookie;
        r.defNorm = r.toNorm(pi.default_value);
        ParamInfo p;
        p.id = pi.id;
        p.title = std::string(pi.name, strnlen(pi.name, sizeof pi.name));
        p.def = r.defNorm;
        const double span = pi.max_value - pi.min_value;
        if ((pi.flags & CLAP_PARAM_IS_STEPPED) && span >= 1 && span <= 4096) p.stepCount = static_cast<int>(std::lround(span));
        if (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) p.flags |= kParamAutomatable;
        if (pi.flags & CLAP_PARAM_IS_HIDDEN) p.flags |= kParamHidden;
        if (pi.flags & CLAP_PARAM_IS_READONLY) p.flags |= kParamReadOnly;
        if (pi.flags & CLAP_PARAM_IS_BYPASS) {
          p.flags |= kParamBypass;
          if (bypassId_ < 0) bypassId_ = pi.id;
        }
        if (pi.flags & CLAP_PARAM_IS_MODULATABLE) {
          p.flags |= kParamModulatable;
          r.modulatable = true;
        }
        params2_.push_back(std::move(p));
        ranges_.push_back(r);
      }
    }
    std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.id < b.id; });
    indexParams();
    cache_.reset(new std::atomic<double>[std::max<size_t>(1, ranges_.size())]);
    for (size_t i = 0; i < ranges_.size(); ++i) cache_[i].store(ranges_[i].toPlain(ranges_[i].defNorm), std::memory_order_relaxed);
    refreshCache();
  }
  const Range* range(uint32_t id) const {
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), id, [](const Range& r, uint32_t v) { return r.id < v; });
    return it != ranges_.end() && it->id == id ? &*it : nullptr;
  }

  void readPorts() {
    inCh_.clear();
    outCh_.clear();
    outNames_.clear();
    auxOut_.clear();
    mainIn_ = mainOut_ = scIn_ = -1;
    if (audioPorts_) {
      for (int dir = 0; dir < 2; ++dir) {
        const bool input = dir == 0;
        const uint32_t n = std::min<uint32_t>(audioPorts_->count(plugin_, input), 32);
        for (uint32_t i = 0; i < n; ++i) {
          clap_audio_port_info_t pi{};
          if (!audioPorts_->get(plugin_, i, input, &pi)) pi.channel_count = 0;
          const int ch = static_cast<int>(std::min<uint32_t>(pi.channel_count, 32));
          const bool main = (pi.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
          if (input) {
            if (main && mainIn_ < 0) mainIn_ = static_cast<int>(i);
            else if (scIn_ < 0 && ch > 0) scIn_ = static_cast<int>(i);
            inCh_.push_back(ch);
          } else {
            if (main && mainOut_ < 0) mainOut_ = static_cast<int>(i);
            outCh_.push_back(ch);
            outNames_.push_back(std::string(pi.name, strnlen(pi.name, sizeof pi.name)));
          }
        }
      }
      if (mainOut_ < 0 && !outCh_.empty()) mainOut_ = 0;
      if (mainIn_ < 0 && scIn_ == 0 && inCh_.size() == 1) {  // single unflagged input: the main one
        mainIn_ = 0;
        scIn_ = -1;
      }
      for (size_t i = 0; i < outCh_.size(); ++i)
        if (static_cast<int>(i) != mainOut_ && outCh_[i] > 0) auxOut_.push_back(static_cast<int>(i));
    }
    noteIn_ = false;
    clapDialect_ = midiDialect_ = false;
    notePort_ = 0;
    noteOut_ = notePorts_ && notePorts_->count(plugin_, false) > 0;
    if (notePorts_ && notePorts_->count(plugin_, true) > 0) {
      clap_note_port_info_t ni{};
      if (notePorts_->get(plugin_, 0, true, &ni)) {
        noteIn_ = true;
        clapDialect_ = (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        midiDialect_ = (ni.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
      }
    }
  }

  void allocate() {
    size_t total = 0;
    for (int c : inCh_) total += static_cast<size_t>(c);
    for (int c : outCh_) total += static_cast<size_t>(c);
    storage_.assign(total * static_cast<size_t>(maxBlock_), 0.0f);
    float* cursor = storage_.data();
    auto wire = [&](std::vector<int>& ch, std::vector<clap_audio_buffer_t>& buf, std::vector<std::vector<float*>>& ptr) {
      buf.assign(ch.size(), clap_audio_buffer_t{});
      ptr.assign(ch.size(), {});
      for (size_t b = 0; b < ch.size(); ++b) {
        ptr[b].resize(static_cast<size_t>(std::max(1, ch[b])), nullptr);
        for (int c = 0; c < ch[b]; ++c) {
          ptr[b][static_cast<size_t>(c)] = cursor;
          cursor += maxBlock_;
        }
        buf[b].data32 = ptr[b].data();
        buf[b].channel_count = static_cast<uint32_t>(ch[b]);
      }
    };
    wire(inCh_, inBuf_, inPtr_);
    wire(outCh_, outBuf_, outPtr_);
  }
  static float* port(std::vector<std::vector<float*>>& ptr, const std::vector<int>& ch, int p, int c) {
    return p >= 0 && static_cast<size_t>(p) < ptr.size() && c >= 0 && c < ch[static_cast<size_t>(p)] ? ptr[static_cast<size_t>(p)][static_cast<size_t>(c)] : nullptr;
  }

  std::shared_ptr<ClapModule> mod_;
  clap_host_t host_{};
  const clap_plugin_t* plugin_ = nullptr;
  const clap_plugin_descriptor_t* desc_ = nullptr;
  const clap_plugin_params_t* params_ = nullptr;
  const clap_plugin_state_t* state_ = nullptr;
  const clap_plugin_latency_t* latencyExt_ = nullptr;
  const clap_plugin_tail_t* tailExt_ = nullptr;
  const clap_plugin_gui_t* gui_ = nullptr;
  const clap_plugin_audio_ports_t* audioPorts_ = nullptr;
  const clap_plugin_note_ports_t* notePorts_ = nullptr;
  const clap_plugin_timer_support_t* timerExt_ = nullptr;
  const clap_plugin_render_t* render_ = nullptr;
  const clap_plugin_preset_load_t* presetLoad_ = nullptr;
  std::thread::id mainThread_;

  std::vector<ParamInfo> params2_;
  std::vector<Range> ranges_;  // sorted by id
  std::unique_ptr<std::atomic<double>[]> cache_;  // plain value per ranges_ entry
  mutable std::atomic<bool> cacheStale_{false};
  int64_t bypassId_ = -1;
  std::atomic<int> pendingBypass_{-1};
  bool noteOut_ = false;
  bool inProcess_ = false;  // audio thread: inside plugin->process
  MidiOutBuffer midiOut_;
  std::vector<int> inCh_, outCh_;
  std::vector<std::string> outNames_;
  std::vector<int> auxOut_;
  int mainIn_ = -1, mainOut_ = -1, scIn_ = -1;
  bool noteIn_ = false, clapDialect_ = false, midiDialect_ = false;
  int notePort_ = 0;
  std::vector<float> storage_;
  std::vector<clap_audio_buffer_t> inBuf_, outBuf_;
  std::vector<std::vector<float*>> inPtr_, outPtr_;

  double sampleRate_ = 0;
  int maxBlock_ = 0;
  bool active_ = false, processing_ = false;
  std::atomic<int> latency_{0};

  InEvents in_;
  clap_output_events_t outEvents_{};
  HostEventQueue events_;
  std::atomic<int32_t> restartFlags_{0};
  std::atomic<bool> dirty_{false}, callback_{false}, flush_{false};
  std::vector<Timer> timers_;
  clap_id nextTimer_ = 1;
};

ClapGui::~ClapGui() {
  if (b_->editor_ == this) b_->editorGone();
}
bool ClapGui::size(int& w, int& h) {
  uint32_t uw = 640, uh = 420;
  if (!created || !b_->gui()->get_size(b_->plugin(), &uw, &uh)) return false;
  w = std::max<int>(64, static_cast<int>(uw));
  h = std::max<int>(48, static_cast<int>(uh));
  return true;
}
bool ClapGui::canResize() const { return created && b_->gui()->can_resize(b_->plugin()); }
void ClapGui::constrain(int& w, int& h) {
  uint32_t uw = static_cast<uint32_t>(std::max(1, w)), uh = static_cast<uint32_t>(std::max(1, h));
  if (created && b_->gui()->adjust_size(b_->plugin(), &uw, &uh)) {
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
  }
}
bool ClapGui::attach(void* parent) {
  if (!created || !parent) return false;
  clap_window_t win{};
  win.api = ClapBackend::windowApi();
#if defined(__APPLE__) || defined(_WIN32)
  win.ptr = parent;
#else
  win.x11 = static_cast<clap_xwnd>(reinterpret_cast<uintptr_t>(parent));
#endif
  if (!b_->gui()->set_parent(b_->plugin(), &win)) return false;
  b_->gui()->show(b_->plugin());
  return true;
}
void ClapGui::setContentScale(double scale) {
#if !defined(__APPLE__)  // cocoa views are in points: CLAP says to ignore the scale there
  if (created && b_->gui()->set_scale) b_->gui()->set_scale(b_->plugin(), scale);
#else
  (void)scale;
#endif
}
void ClapGui::setSize(int w, int h) {
  if (created) b_->gui()->set_size(b_->plugin(), static_cast<uint32_t>(std::max(1, w)), static_cast<uint32_t>(std::max(1, h)));
}

// ---------------------------------------------------------------- preset discovery

// Indexes the presets of every provider in the plugin's module (its
// preset-discovery factory) and keeps those that apply to this plugin.
std::vector<PresetRef> ClapBackend::discoverPresets() {
  std::vector<PresetRef> out;
  const clap_plugin_entry_t* entry = mod_ ? mod_->entry : nullptr;
  if (!entry) return out;
  auto* f = static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
  if (!f) f = static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
  if (!f) return out;
  struct Loc {
    uint32_t kind = 0;
    std::string name, path;
  };
  struct Index {
    std::vector<std::string> exts;
    std::vector<Loc> locs;
  } idx;
  clap_preset_discovery_indexer_t ix{};
  ix.clap_version = CLAP_VERSION;
  ix.name = "Gridline";
  ix.vendor = "Gridline";
  ix.url = "";
  ix.version = "0.5";
  ix.indexer_data = &idx;
  ix.declare_filetype = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_filetype_t* ft) {
    if (ft && ft->file_extension) static_cast<Index*>(i->indexer_data)->exts.push_back(ft->file_extension);
    return true;
  };
  ix.declare_location = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* l) {
    if (!l) return false;
    Loc x;
    x.kind = l->kind;
    x.name = l->name ? l->name : "";
    x.path = l->location ? l->location : "";
    static_cast<Index*>(i->indexer_data)->locs.push_back(std::move(x));
    return true;
  };
  ix.declare_soundpack = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) { return true; };
  ix.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };

  struct Rx {
    std::vector<PresetRef>* out = nullptr;
    std::string pluginId;
    PresetRef cur;
    bool open = false, anyId = false, match = false;
    uint32_t kind = 0;
    std::string location;
    void commit() {
      if (open && (!anyId || match) && out->size() < 20000) out->push_back(cur);
      open = false;
    }
  } rx;
  rx.out = &out;
  rx.pluginId = desc_ && desc_->id ? desc_->id : "";
  clap_preset_discovery_metadata_receiver_t mr{};
  mr.receiver_data = &rx;
  mr.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char* msg) {
    if (msg) std::fprintf(stderr, "[clap] preset index: %s\n", msg);
  };
  mr.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
    Rx& x = *static_cast<Rx*>(r->receiver_data);
    x.commit();
    x.cur = PresetRef{};
    x.cur.name = name ? name : "";
    x.cur.key = key ? key : "";
    x.cur.location = x.location;
    x.cur.locationKind = x.kind;
    if (x.cur.name.empty()) x.cur.name = std::filesystem::path(x.location).stem().string();
    x.open = true;
    x.anyId = x.match = false;
    return true;
  };
  mr.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
    Rx& x = *static_cast<Rx*>(r->receiver_data);
    if (!id) return;
    x.anyId = true;
    if (id->abi && id->id && !std::strcmp(id->abi, "clap") && x.pluginId == id->id) x.match = true;
  };
  mr.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
  mr.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
  mr.add_creator = [](const clap_preset_discovery_metadata_receiver_t* r, const char* c) {
    Rx& x = *static_cast<Rx*>(r->receiver_data);
    if (c && x.cur.creator.empty()) x.cur.creator = c;
  };
  mr.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
  mr.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
  mr.add_feature = [](const clap_preset_discovery_metadata_receiver_t* r, const char* feat) {
    Rx& x = *static_cast<Rx*>(r->receiver_data);
    if (feat && x.cur.category.empty()) x.cur.category = feat;
  };
  mr.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};

  const uint32_t providers = std::min<uint32_t>(f->count(f), 64);
  for (uint32_t i = 0; i < providers; ++i) {
    const clap_preset_discovery_provider_descriptor_t* d = f->get_descriptor(f, i);
    if (!d || !d->id) continue;
    idx = Index{};
    const clap_preset_discovery_provider_t* prov = f->create(f, &ix, d->id);
    if (!prov) continue;
    if (!prov->init(prov)) {
      prov->destroy(prov);
      continue;
    }
    for (const Loc& loc : idx.locs) {
      auto scan = [&](const std::string& path) {
        rx.kind = loc.kind;
        rx.location = path;
        prov->get_metadata(prov, loc.kind, loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : path.c_str(), &mr);
        rx.commit();
      };
      if (loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        scan(std::string());
        continue;
      }
      std::error_code ec;
      if (std::filesystem::is_directory(loc.path, ec)) {
        int files = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(loc.path, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator() && files < 5000; it.increment(ec)) {
          if (!it->is_regular_file(ec)) continue;
          std::string ext = it->path().extension().string();
          if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
          if (!idx.exts.empty() && std::find(idx.exts.begin(), idx.exts.end(), ext) == idx.exts.end()) continue;
          ++files;
          scan(it->path().string());
        }
      } else if (std::filesystem::exists(loc.path, ec)) {
        scan(loc.path);
      }
    }
    prov->destroy(prov);
  }
  std::stable_sort(out.begin(), out.end(), [](const PresetRef& a, const PresetRef& b) {
    return a.category != b.category ? a.category < b.category : a.name < b.name;
  });
  return out;
}

// ---------------------------------------------------------------- format

std::vector<std::string> clapPaths() {
  std::vector<std::string> v;
  const char* home = gl::platform::homeEnv();
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
  v.push_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
  (void)home;
  addSystemPluginFolders(v, "CLAP", "clap");
#else
  if (home) v.push_back(std::string(home) + "/.clap");
  addSystemPluginFolders(v, "CLAP", "clap");
#endif
  addEnvPaths(v, "CLAP_PATH");
  return v;
}

std::vector<std::string> clapDiscover(const std::vector<std::string>& folders) {
#if defined(__APPLE__)
  return findEntries(folders, ".clap", true, false);
#else
  return findEntries(folders, ".clap", false, true);
#endif
}

ScanResult clapScan(const std::string& path) {
  ScanResult r;
  r.format = "clap";
  r.bundle = path;
  r.mtime = pathMtime(path);
  std::string err;
  auto mod = loadModule(path, &err);
  if (!mod) {
    r.error = err;
    return r;
  }
  const uint32_t n = mod->factory->get_plugin_count(mod->factory);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_plugin_descriptor_t* d = mod->factory->get_plugin_descriptor(mod->factory, i);
    if (!d || !d->id) continue;
    ScannedClass c;
    c.uid = d->id;
    c.name = d->name ? d->name : d->id;
    c.vendor = d->vendor ? d->vendor : "";
    c.version = d->version ? d->version : "";
    c.sdkVersion = "CLAP " + std::to_string(d->clap_version.major) + "." + std::to_string(d->clap_version.minor) + "." +
                   std::to_string(d->clap_version.revision);
    bool instrument = false, fx = false;
    if (d->features)
      for (const char* const* f = d->features; *f; ++f) {
        if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT)) instrument = true;
        if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)) fx = true;
        c.subCategories += (c.subCategories.empty() ? "" : "|") + std::string(*f);
      }
    std::string perr;
    auto b = ClapBackend::create(path, c.uid, &perr);
    if (!b) {
      r.error += (r.error.empty() ? "" : "; ") + c.name + ": " + perr;
      continue;
    }
    c.eventInput = b->hasEventInput();
    c.eventOutput = b->hasEventOutput();
    if (b->prepare(48000.0, 512)) {
      c.inputChannels = b->mainInputChannels();
      c.outputChannels = b->mainOutputChannels();
      c.sidechainChannels = b->sidechainChannels();
      c.latency = b->latency();
      for (int k = 0; k < b->auxOutputCount(); ++k) c.auxOutputs.push_back(b->auxOutputName(k));
    }
    c.instrument = instrument || (!fx && c.eventInput && c.inputChannels == 0);
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
    (void)b.release();  // never destroyed in the scanner (plugins that crash on destroy must not fail the scan)
  }
  r.ok = !r.classes.empty();
  if (!r.ok && r.error.empty()) r.error = "no plugins in the factory";
  return r;
}

std::unique_ptr<Backend> clapCreate(const std::string& path, const std::string& id, std::string* err) {
  return ClapBackend::create(path, id, err);
}

}  // namespace

Format clapFormat() {
  Format f{};
  f.id = "clap";
  f.label = "CLAP";
  f.defaultPaths = clapPaths;
  f.discover = clapDiscover;
  f.scanInProcess = clapScan;
  f.create = clapCreate;
  f.mtime = pathMtime;
  return f;
}

}  // namespace gl::host

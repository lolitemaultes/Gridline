// Gridline — "seltmann" pack: Seltmann runtime. Pack entry points, the data the
// audio thread reads (SeltmannAux, built on the UI thread), the warp service
// (the region time-stretched to the project tempo on a worker thread, handed
// over through an atomic pointer), the instrument (slices on keys, the
// pattern on the octave below, voices with declick fades, ADSR, choke groups,
// mono, stutter, swing) and the offline renders (loops, slices, spans, notes).
//
// Audio thread: no allocation, no locks, no I/O. Voices are a fixed array; the
// aux data never changes once built; warp renders are retired with a grace
// period long after the audio thread could still read them.
#include "../Packs.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../../core/AudioBuffer.h"
#include "../../core/MidiFile.h"
#include "../../core/Song.h"
#include "../../core/WavWriter.h"
#include "../../engine/TimeStretch.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

namespace {

float par(const PluginState& s, int i, float def) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : def; }
int ipar(const PluginState& s, int i, int def) { return static_cast<int>(std::lround(par(s, i, static_cast<float>(def)))); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ================================================================== warp service
struct Render {
  double tempo = 0;
  double rate = 44100;
  std::vector<float> L, R;
  int64_t frames() const { return static_cast<int64_t>(L.size()); }
};

struct WarpSlot {
  // inputs (immutable)
  SamplePtr src;
  Analysis grid;  // beats only
  int64_t a = 0, b = 0;
  double beatA = 0, beatB = 0;
  int mode = 0;
  // audio -> worker
  std::atomic<double> wanted{0};
  // worker -> audio
  std::atomic<const Render*> current{nullptr};
  // worker only
  double lastWanted = 0;
  int stable = 0;
  std::vector<std::unique_ptr<Render>> owned;
  std::vector<std::pair<std::chrono::steady_clock::time_point, std::unique_ptr<Render>>> retired;
};

stretch::Mode modeOf(int m) {
  switch (m) {
    case 1: return stretch::Mode::Transient;
    case 2: return stretch::Mode::Mono;
    default: return stretch::Mode::Auto;
  }
}

std::unique_ptr<Render> makeRender(const WarpSlot& w, double tempo) {
  if (!w.src || w.b <= w.a || tempo <= 0) return nullptr;
  auto r = std::make_unique<Render>();
  r->tempo = tempo;
  r->rate = w.src->sampleRate;
  const double outBeat = 60.0 / tempo * r->rate;  // output frames per beat
  const int64_t outLen = std::max<int64_t>(1, static_cast<int64_t>(std::llround((w.beatB - w.beatA) * outBeat)));
  if (outLen > static_cast<int64_t>(r->rate * 60 * 12)) return nullptr;  // 12 minutes: refuse
  r->L.assign(static_cast<size_t>(outLen), 0.f);
  r->R.assign(static_cast<size_t>(outLen), 0.f);
  const float* in[2] = {w.src->left.data() + w.a, w.src->right.data() + w.a};
  float* out[2] = {r->L.data(), r->R.data()};
  stretch::Settings st;
  st.mode = modeOf(w.mode);
  st.pitch = 1;
  const Analysis& g = w.grid;
  const double beatA = w.beatA;
  const int64_t a = w.a, n = w.b - w.a;
  stretch::processMapped(in, 2, n, out, outLen, r->rate, st, [&](int64_t j) {
    return std::clamp(frameAt(g, beatA + static_cast<double>(j) / outBeat) - static_cast<double>(a), 0.0, static_cast<double>(n - 1));
  });
  return r;
}

class WarpService {
 public:
  static WarpService& get() {
    static WarpService s;
    return s;
  }
  std::shared_ptr<WarpSlot> slot(const std::string& key, const std::function<std::shared_ptr<WarpSlot>()>& make) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(key);
    if (it != slots_.end())
      if (auto s = it->second.lock()) return s;
    auto s = make();
    slots_[key] = s;
    if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    return s;
  }
  ~WarpService() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
      cv_.wait_for(lk, std::chrono::milliseconds(30));
      if (stop_) break;
      std::vector<std::shared_ptr<WarpSlot>> live;
      for (auto it = slots_.begin(); it != slots_.end();) {
        if (auto s = it->second.lock()) {
          live.push_back(std::move(s));
          ++it;
        } else {
          it = slots_.erase(it);
        }
      }
      lk.unlock();
      const auto now = std::chrono::steady_clock::now();
      for (auto& s : live) {
        // free retired renders after a long grace period (the audio thread reads one per block)
        for (auto r = s->retired.begin(); r != s->retired.end();) {
          if (now - r->first > std::chrono::seconds(10)) r = s->retired.erase(r);
          else ++r;
        }
        const double w = s->wanted.load(std::memory_order_relaxed);
        const Render* cur = s->current.load(std::memory_order_acquire);
        if (w <= 0 || (cur && std::fabs(cur->tempo - w) < 1e-4)) continue;
        if (w == s->lastWanted) ++s->stable;
        else s->stable = 0, s->lastWanted = w;
        if (s->stable < 2) continue;  // wait until the tempo settles
        // reuse an owned render of that tempo
        Render* found = nullptr;
        for (auto& o : s->owned)
          if (std::fabs(o->tempo - w) < 1e-4) found = o.get();
        if (!found) {
          auto r = makeRender(*s, w);
          if (!r) {
            s->wanted = 0;
            continue;
          }
          found = r.get();
          s->owned.push_back(std::move(r));
          // keep at most four renders; older ones retire (freed after the grace period)
          while (s->owned.size() > 4) {
            auto& o = s->owned.front();
            if (o.get() == s->current.load() ) break;
            s->retired.push_back({now, std::move(o)});
            s->owned.erase(s->owned.begin());
          }
        }
        s->current.store(found, std::memory_order_release);
      }
      live.clear();
      lk.lock();
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::weak_ptr<WarpSlot>> slots_;
  std::thread thread_;
  bool stop_ = false;
};

// ================================================================== aux
struct RtSlice {
  int64_t a = 0, b = 0;  // playable source frames (offset / length applied)
  double beatA = 0, beatB = 0;
  float gain = 1, pitch = 0;
  bool reverse = false;
  float fadeIn = 0, fadeOut = 0;  // seconds
  int choke = 0;
};
struct RtStep {
  int slice = -1;
  double unitStart = 0;  // units from the pattern start
  int units = 1;
  float pitch = 0, gain = 1, length = 1, offset = 0;
  bool reverse = false;
  int stutter = 1;
};

struct SeltmannAux final : AuxData {
  SamplePtr src;
  Analysis grid;
  double srcBpm = 120;
  std::vector<RtSlice> slices;
  std::vector<RtStep> steps;
  std::array<int, kSlotCount> slots{};
  double unitBeats = 1, patUnits = 0;
  std::shared_ptr<WarpSlot> warp;
};

uint64_t hashBeats(const Analysis& g) {
  uint64_t h = 1469598103934665603ull;
  for (double b : g.beats) {
    const auto v = static_cast<uint64_t>(std::llround(b * 16));
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

std::shared_ptr<SeltmannAux> buildAux(const Model& m, SamplePtr src, const std::string& path, int stretchMode, bool registerWarp) {
  auto x = std::make_shared<SeltmannAux>();
  x->src = src;
  x->grid = m.grid;
  x->grid.onsetEnv.clear();
  x->grid.sections.clear();
  x->grid.loops.clear();
  x->srcBpm = m.grid.bpm > 1 ? m.grid.bpm : 120.0;
  const int64_t nFrames = src ? static_cast<int64_t>(src->frames()) : m.srcFrames;
  for (const auto& s : m.slices) {
    RtSlice r;
    const int64_t len = std::max<int64_t>(1, s.end - s.start);
    r.a = std::clamp<int64_t>(s.start + static_cast<int64_t>(len * std::clamp(s.offset, 0.0f, 0.9f)), 0, std::max<int64_t>(0, nFrames - 1));
    r.b = std::clamp<int64_t>(r.a + static_cast<int64_t>(len * std::clamp(s.length, 0.05f, 1.0f)), r.a + 1, std::max<int64_t>(r.a + 1, std::min(s.end, nFrames)));
    r.beatA = beatAt(m.grid, static_cast<double>(r.a));
    r.beatB = beatAt(m.grid, static_cast<double>(r.b));
    r.gain = static_cast<float>(dbToGain(s.gain));
    r.pitch = s.pitch;
    r.reverse = s.reverse;
    r.fadeIn = s.fadeIn * 0.001f;
    r.fadeOut = s.fadeOut * 0.001f;
    r.choke = s.choke;
    x->slices.push_back(r);
  }
  const int n = static_cast<int>(x->slices.size());
  double u = 0;
  for (const auto& st : m.pattern.steps) {
    RtStep r;
    r.slice = stepSlice(m.pattern, st, n);
    r.unitStart = u;
    r.units = 1 + st.ties;
    r.pitch = st.pitch;
    r.gain = static_cast<float>(dbToGain(st.gain));
    r.length = st.length;
    r.offset = st.offset;
    r.reverse = st.reverse;
    r.stutter = std::clamp(st.stutter, 1, 16);
    x->steps.push_back(r);
    u += r.units;
  }
  x->patUnits = u;
  x->unitBeats = std::max(1.0 / 64, m.pattern.unitBeats);
  x->slots.fill(-1);
  for (int k = 0; k < kSlotCount; ++k) {
    const int v = k < static_cast<int>(m.pattern.slots.size()) ? m.pattern.slots[static_cast<size_t>(k)] : -1;
    x->slots[static_cast<size_t>(k)] = v >= 0 && v < n ? v : (n > 0 ? k % n : -1);
  }
  if (src && m.regionEnd > m.regionStart) {
    auto make = [&] {
      auto w = std::make_shared<WarpSlot>();
      w->src = src;
      w->grid = x->grid;
      w->a = std::clamp<int64_t>(m.regionStart, 0, nFrames);
      w->b = std::clamp<int64_t>(m.regionEnd, w->a, nFrames);
      w->beatA = beatAt(m.grid, static_cast<double>(w->a));
      w->beatB = beatAt(m.grid, static_cast<double>(w->b));
      w->mode = stretchMode;
      return w;
    };
    if (registerWarp) {
      const std::string key = path + "|" + std::to_string(m.regionStart) + "|" + std::to_string(m.regionEnd) + "|" + std::to_string(stretchMode) + "|" +
                              std::to_string(hashBeats(m.grid)) + "|" + std::to_string(reinterpret_cast<uintptr_t>(src.get()));
      x->warp = WarpService::get().slot(key, make);
    } else {
      x->warp = make();
    }
  }
  return x;
}

// Model of a state: the stored one, or a default one from the analysis (or a flat grid).
bool modelOf(const PluginState& st, SamplePtr src, std::string& path, Model& m) {
  if (parseData(st.data, path, m) && m.srcFrames > 0) {
    if (src && m.srcFrames != static_cast<int64_t>(src->frames())) {
      // the file changed under the model: keep nothing that points into it
    } else {
      return true;
    }
  }
  path = pathOf(st.data);
  if (!src) return false;
  JobPtr job = requestAnalysis(path);
  if (job->state == 0 && src->seconds() < 40) {
    // short material: wait for the (fast) analysis rather than starting on a guessed grid
    for (int i = 0; i < 4000 && job->state == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  m = defaultModel(*src, job->state == 1 && job->result.frames == static_cast<int64_t>(src->frames()) ? job->result : flatGrid(*src));
  return true;
}

// ================================================================== instrument
class SeltmannGen final : public Generator {
 public:
  SeltmannGen() { (void)stretch::sincTable(stretch::Quality::Realtime); }

  bool active() const override {
    if (patOn_) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  bool handlesNotePan() const override { return false; }
  int displayStep() const override { return display_.load(std::memory_order_relaxed); }
  double liveFrame() const { return liveFrame_.load(std::memory_order_relaxed); }
  double livePattern() const { return livePat_.load(std::memory_order_relaxed); }
  bool patternRunning() const { return patRun_.load(std::memory_order_relaxed); }

  // Offline renders: a private warp render to use instead of the service.
  void forceRender(const Render* r) { forced_ = r; }

 protected:
  struct Voice {
    bool on = false, held = false, pattern = false;
    uint32_t id = 0;
    int slice = -1, choke = 0;
    const float* L = nullptr;
    const float* R = nullptr;
    int64_t n = 0;
    double pos = 0, lo = 0, hi = 0, rate = 1;
    int dir = 1;
    bool warped = false;
    double gain = 1;
    double played = 0;       // output samples since the start
    double left = -1;        // output samples until the gate closes (-1 = while held)
    double fadeInS = 1, fadeOutS = 1;
    int stage = 0;           // 0 attack, 1 decay, 2 sustain, 3 release
    double env = 0;
    double kill = 1, killStep = 0;
    uint64_t age = 0;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    const auto* nx = dynamic_cast<const SeltmannAux*>(aux.custom.get());
    if (nx != aux_) patPrimed_ = false;  // new chop: find the step again
    aux_ = nx;
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120.0;
    ppq_ = std::max(1, ctx.ppq);
    tick_ = ctx.tick;
    playing_ = ctx.playing;
    jumped_ = ctx.jumped;
    blockOff_ = 0;
    vol_ = std::clamp(static_cast<double>(par(s, SmVolume, 0.8f)), 0.0, 1.5);
    const double pan = std::clamp(static_cast<double>(par(s, SmPan, 0)), -1.0, 1.0);
    panL_ = std::cos((pan + 1) * 0.25 * 3.14159265358979) * 1.41421356;
    panR_ = std::sin((pan + 1) * 0.25 * 3.14159265358979) * 1.41421356;
    base_ = std::clamp(ipar(s, SmBaseNote, 60), 0, 127);
    keyMap_ = std::clamp(ipar(s, SmKeyMap, KeySlices), 0, 1);
    sync_ = std::clamp(ipar(s, SmSync, SyncStretch), 0, 2);
    lock_ = par(s, SmLock, 1) >= 0.5f;
    mono_ = par(s, SmMono, 0) >= 0.5f;
    attack_ = std::max(0.0, static_cast<double>(par(s, SmAttack, 0))) * 0.001;
    decay_ = std::max(0.0, static_cast<double>(par(s, SmDecay, 0))) * 0.001;
    sustain_ = std::clamp(static_cast<double>(par(s, SmSustain, 1)), 0.0, 1.0);
    release_ = std::max(0.0, static_cast<double>(par(s, SmRelease, 10))) * 0.001;
    declick_ = std::clamp(static_cast<double>(par(s, SmDeclick, 3)), 0.3, 50.0) * 0.001;
    swing_ = std::clamp(static_cast<double>(par(s, SmSwing, 0)), 0.0, 1.0);
    pitch_ = std::clamp(static_cast<double>(par(s, SmPitch, 0)), -48.0, 48.0);
    velSens_ = std::clamp(static_cast<double>(par(s, SmVelocity, 0.6f)), 0.0, 1.0);
    reverseAll_ = par(s, SmReverse, 0) >= 0.5f;
    gate_ = std::clamp(static_cast<double>(par(s, SmGate, 1)), 0.05, 1.0);
    const int sp = std::clamp(ipar(s, SmSpeed, SpeedNormal), 0, 2);
    speed_ = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
    const double cut = std::clamp(static_cast<double>(par(s, SmCutoff, 20000)), 20.0, 20000.0);
    reso_ = std::clamp(static_cast<double>(par(s, SmReso, 0)), 0.0, 1.0);
    filterOn_ = cut < 19900.0;
    if (filterOn_) {
      g_ = std::tan(3.14159265358979 * std::min(cut, sr_ * 0.45) / sr_);
      k_ = 2.0 - 1.9 * reso_;
    }
    // warp: ask for this tempo; use the render when it matches
    render_ = nullptr;
    if (aux_ && sync_ == SyncStretch) {
      if (forced_) {
        render_ = forced_;
      } else if (aux_->warp) {
        aux_->warp->wanted.store(tempo_, std::memory_order_relaxed);
        const Render* r = aux_->warp->current.load(std::memory_order_acquire);
        if (r && std::fabs(r->tempo - tempo_) < 1e-4) render_ = r;
      }
    }
  }

  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& p) override {
    noteOnImpl(id, pitch, vel, std::round((p.modX - 0.5) * 48.0), p.modY > 0.75f);
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override { noteOnImpl(id, pitch, vel, 0, false); }

  void onNoteOff(uint32_t id) override {
    if (patOn_ && id == patId_) {
      setPat(false);
      for (auto& v : voices_)
        if (v.on && v.pattern) release(v);
      display_.store(-1, std::memory_order_relaxed);
    }
    for (auto& v : voices_)
      if (v.on && !v.pattern && v.id == id && v.held) {
        v.held = false;
        release(v);
      }
  }
  // J-112: the pattern is the instrument's own playback (latched by its key,
  // from the editor's Play or a keyboard): Play with the transport stopped
  // ends it with a normal release, like the key's release.
  bool ownPlayback() const override { return patOn_; }
  void stopOwnPlayback() override {
    if (!patOn_) return;
    setPat(false);
    for (auto& v : voices_)
      if (v.on && v.pattern) release(v);
    display_.store(-1, std::memory_order_relaxed);
  }
  void onCut(uint32_t id) override {
    if (patOn_ && id == patId_) setPat(false);
    for (auto& v : voices_)
      if (v.on && (v.id == id || v.pattern)) startKill(v);
  }
  void onAllOff() override {
    setPat(false);
    for (auto& v : voices_)
      if (v.on) release(v);
  }
  void onKill() override {
    setPat(false);
    for (auto& v : voices_) v.on = false;
    display_.store(-1, std::memory_order_relaxed);
  }

  void render(float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i) {
      if (patOn_ && aux_ && !aux_->steps.empty()) patternTick(blockOff_ + i, i);
      double l = 0, r = 0;
      for (auto& v : voices_)
        if (v.on) voiceSample(v, l, r);
      if (filterOn_) {
        l = svf(l, ic1L_, ic2L_);
        r = svf(r, ic1R_, ic2R_);
      }
      L[i] += static_cast<float>(l * vol_ * panL_);
      R[i] += static_cast<float>(r * vol_ * panR_);
    }
    blockOff_ += n;
    publish();
    if (!patOn_) {
      bool any = false;
      for (const auto& v : voices_) any = any || v.on;
      if (!any) display_.store(-1, std::memory_order_relaxed);
    }
  }

 private:
  // ---- note handling
  void noteOnImpl(uint32_t id, double pitch, float vel, double extraSemis, bool extraReverse) {
    const int note = static_cast<int>(std::lround(pitch));
    const double fine = pitch - note;
    if (note >= base_ - 12 && note < base_) {  // pattern keys (latched even before the prepared data arrives)
      setPat(true);
      patId_ = id;
      patSemis_ = note - (base_ - 12) + fine;
      patVel_ = vel;
      patElapsed_ = 0;
      patPrimed_ = false;
      return;
    }
    if (!aux_) return;
    const int k = note - base_;
    int slice = -1;
    if (keyMap_ == KeySlots) slice = k >= 0 && k < kSlotCount ? aux_->slots[static_cast<size_t>(k)] : -1;
    else slice = k;
    if (slice < 0 || slice >= static_cast<int>(aux_->slices.size())) return;
    startVoice(id, slice, fine + extraSemis, vel, 1.0, extraReverse, 0.0, -1, false);
    display_.store(packDisplay(-1, slice), std::memory_order_relaxed);
  }

  // Source buffer and frame mapping for the current sync mode.
  struct Src {
    const float* L = nullptr;
    const float* R = nullptr;
    int64_t n = 0;
    double rate = 1;  // input frames per output frame before pitch
    bool warped = false;
  };
  Src source() const {
    Src s;
    if (!aux_ || !aux_->src) return s;
    if (render_) {
      s.L = render_->L.data();
      s.R = render_->R.data();
      s.n = render_->frames();
      s.rate = render_->rate / sr_;
      s.warped = true;
      return s;
    }
    const Sample& smp = *aux_->src;
    s.L = smp.left.data();
    s.R = smp.right.data();
    s.n = static_cast<int64_t>(smp.frames());
    const double sp = sync_ == SyncOriginal ? 1.0 : tempo_ / aux_->srcBpm;
    s.rate = smp.sampleRate / sr_ * sp;
    return s;
  }
  // Source frame -> buffer frame (warped renders map beats onto the project tempo).
  double mapFrame(const Src& s, double beat, double frame) const {
    if (!s.warped || !aux_->warp) return frame;
    return (beat - aux_->warp->beatA) * 60.0 / render_->tempo * render_->rate;
  }

  // Starts a slice. gateSamples < 0: plays while held (keys), else for that long (pattern steps).
  void startVoice(uint32_t id, int slice, double semis, float vel, double stepGain, bool stepReverse, double stepOffset, double gateSamples,
                  bool pattern) {
    const RtSlice& sl = aux_->slices[static_cast<size_t>(slice)];
    const Src s = source();
    if (!s.L || s.n <= 0) return;
    // chokes: mono, the slice's group
    for (auto& v : voices_)
      if (v.on && (mono_ || (sl.choke > 0 && v.choke == sl.choke))) startKill(v);
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick) {  // steal the oldest
      pick = &voices_[0];
      for (auto& v : voices_)
        if (v.age < pick->age) pick = &v;
    }
    Voice& v = *pick;
    v = Voice{};
    v.on = true;
    v.held = gateSamples < 0;
    v.pattern = pattern;
    v.id = id;
    v.slice = slice;
    v.choke = sl.choke;
    v.L = s.L;
    v.R = s.R;
    v.n = s.n;
    v.warped = s.warped;
    v.lo = std::clamp(mapFrame(s, sl.beatA, static_cast<double>(sl.a)), 0.0, static_cast<double>(s.n - 1));
    v.hi = std::clamp(mapFrame(s, sl.beatB, static_cast<double>(sl.b)), v.lo + 1, static_cast<double>(s.n));
    if (stepOffset > 0) v.lo = std::min(v.hi - 1, v.lo + (v.hi - v.lo) * stepOffset);
    v.rate = s.rate * std::pow(2.0, (sl.pitch + semis + pitch_) / 12.0);
    const bool rev = (sl.reverse != stepReverse) != reverseAll_;
    v.dir = rev ? -1 : 1;
    v.pos = rev ? v.hi - 1 : v.lo;
    const double velGain = 1.0 - velSens_ + velSens_ * std::clamp(static_cast<double>(vel), 0.0, 1.0);
    v.gain = sl.gain * stepGain * velGain;
    v.left = gateSamples;
    v.fadeInS = std::max(1.0, (declick_ + sl.fadeIn) * sr_);
    v.fadeOutS = std::max(1.0, (declick_ + sl.fadeOut) * sr_);
    v.stage = 0;
    v.env = attack_ <= 0 ? 1.0 : 0.0;
    if (attack_ <= 0) v.stage = 1;
    v.age = ++ageCounter_;
  }

  void release(Voice& v) {
    if (v.stage < 3) v.stage = 3;
  }
  void startKill(Voice& v) {
    if (v.killStep <= 0) v.killStep = 1.0 / std::max(1.0, declick_ * sr_);
  }

  void voiceSample(Voice& v, double& outL, double& outR) {
    // amplitude envelope
    switch (v.stage) {
      case 0:
        v.env += 1.0 / std::max(1.0, attack_ * sr_);
        if (v.env >= 1) v.env = 1, v.stage = 1;
        break;
      case 1:
        v.env -= (1.0 - sustain_) / std::max(1.0, std::max(decay_, 0.002) * sr_);
        if (v.env <= sustain_) v.env = sustain_, v.stage = 2;
        break;
      case 2: v.env = sustain_; break;
      default: {
        const double rel = std::max(release_, declick_);
        v.env -= 1.0 / std::max(1.0, rel * sr_);
        if (v.env <= 0) {
          v.on = false;
          return;
        }
        break;
      }
    }
    if (v.killStep > 0) {
      v.kill -= v.killStep;
      if (v.kill <= 0) {
        v.on = false;
        return;
      }
    }
    // gate (pattern steps): the release starts so it ends with the step
    if (v.left >= 0) {
      v.left -= 1;
      if (v.left <= 0 && v.stage < 3) v.stage = 3;
    }
    // edge fades: in from the start, out before the slice edge
    const double dist = (v.dir > 0 ? v.hi - v.pos : v.pos - v.lo) / std::max(1e-9, v.rate);
    if (dist <= 0) {
      v.on = false;
      return;
    }
    const double fin = std::min(1.0, (v.played + 1) / v.fadeInS);
    const double fout = std::min(1.0, dist / v.fadeOutS);
    // declick the gate's own end too (the release may be shorter than the declick)
    const double a = v.env * fin * fout * v.kill * v.gain;
    float l, r;
    if (std::fabs(v.rate - 1.0) < 1e-9 && std::fabs(v.pos - std::floor(v.pos)) < 1e-9) {
      const auto i = static_cast<int64_t>(v.pos);
      l = i >= 0 && i < v.n ? v.L[i] : 0.f;
      r = i >= 0 && i < v.n ? v.R[i] : 0.f;
    } else {
      stretch::readSinc2(v.L, v.R, v.n, v.pos, v.rate, stretch::sincTable(stretch::Quality::Realtime), l, r);
    }
    outL += l * a;
    outR += r * a;
    v.pos += v.dir * v.rate;
    v.played += 1;
  }

  double svf(double x, double& ic1, double& ic2) const {
    const double a1 = 1.0 / (1.0 + g_ * (g_ + k_)), a2 = g_ * a1, a3 = g_ * a2;
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
    return v2;
  }

  // ---- pattern sequencer
  double beatsPerSample() const {
    const double bpm = sync_ == SyncOriginal || !aux_ ? (aux_ ? aux_->srcBpm : 120.0) : tempo_;
    return bpm / 60.0 / sr_ * speed_;
  }
  double eventBeat(int step, int sub) const {
    const RtStep& st = aux_->steps[static_cast<size_t>(step)];
    const double u = aux_->unitBeats;
    double t = st.unitStart * u;
    if (swing_ > 0 && u <= 1.0 + 1e-9 && static_cast<long long>(std::llround(st.unitStart)) % 2 == 1) t += swing_ * 0.5 * u;
    return t + stepLen(step) * sub / st.stutter;
  }
  double stepLen(int step) const {
    const double u = aux_->unitBeats;
    const int n = static_cast<int>(aux_->steps.size());
    auto startOf = [&](int k) {
      if (k >= n) return aux_->patUnits * u;
      const RtStep& s2 = aux_->steps[static_cast<size_t>(k)];
      double t = s2.unitStart * u;
      if (swing_ > 0 && u <= 1.0 + 1e-9 && static_cast<long long>(std::llround(s2.unitStart)) % 2 == 1) t += swing_ * 0.5 * u;
      return t;
    };
    return std::max(1e-6, startOf(step + 1) - startOf(step));
  }
  void seek(double p) {
    // first event at or after p
    const int n = static_cast<int>(aux_->steps.size());
    nextStep_ = n;
    nextSub_ = 0;
    for (int k = 0; k < n && nextStep_ == n; ++k)
      for (int s = 0; s < aux_->steps[static_cast<size_t>(k)].stutter; ++s)
        if (eventBeat(k, s) >= p - 1e-9) {
          nextStep_ = k;
          nextSub_ = s;
          break;
        }
  }
  void advance() {
    const int n = static_cast<int>(aux_->steps.size());
    if (++nextSub_ >= aux_->steps[static_cast<size_t>(nextStep_)].stutter) {
      nextSub_ = 0;
      ++nextStep_;
    }
    if (nextStep_ > n) nextStep_ = n;
  }
  void trigger(int step, int sub) {
    const RtStep& st = aux_->steps[static_cast<size_t>(step)];
    if (sub == 0) display_.store(packDisplay(step, st.slice), std::memory_order_relaxed);
    if (st.slice < 0 || st.slice >= static_cast<int>(aux_->slices.size())) return;
    const double lenBeats = stepLen(step) / st.stutter;
    const double gateSamples = std::max(1.0, lenBeats / beatsPerSample() * gate_ * st.length);
    startVoice(0x80000000u | static_cast<uint32_t>(step), st.slice, patSemis_ + st.pitch, patVel_, st.gain, st.reverse, st.offset, gateSamples, true);
  }
  void patternTick(int64_t off, int) {
    const double patBeats = aux_->patUnits * aux_->unitBeats;
    if (patBeats <= 0) return;
    const double bps = beatsPerSample();
    double p;
    const bool songLock = lock_ && playing_ && sync_ != SyncOriginal;
    if (songLock) {
      const double songBeat = tick_ / ppq_ + static_cast<double>(off) * tempo_ / 60.0 / sr_;
      p = songBeat * speed_;
    } else {
      p = patElapsed_ * bps;
      patElapsed_ += 1;
    }
    p = std::fmod(p, patBeats);
    if (p < 0) p += patBeats;
    if (!patPrimed_ || (jumped_ && off == 0)) {
      // a note placed on a step starts that step: look a hair before the position
      seek(p - 1e-6);
      patPrimed_ = true;
      lastP_ = p;
    } else if (p + 1e-9 < lastP_) {
      // wrapped (or jumped back): finish the cycle, then start over
      if (lastP_ - p > patBeats * 0.5) {
        const int n = static_cast<int>(aux_->steps.size());
        while (nextStep_ < n) {
          trigger(nextStep_, nextSub_);
          advance();
        }
        seek(0);
      } else {
        seek(p);
      }
    }
    const int n = static_cast<int>(aux_->steps.size());
    while (nextStep_ < n && eventBeat(nextStep_, nextSub_) <= p + 1e-9) {
      trigger(nextStep_, nextSub_);
      advance();
    }
    lastP_ = p;
  }

  const SeltmannAux* aux_ = nullptr;
  const Render* render_ = nullptr;
  const Render* forced_ = nullptr;
  std::array<Voice, kMaxVoices> voices_{};
  uint64_t ageCounter_ = 0;
  double sr_ = 44100, tempo_ = 120, tick_ = 0;
  int ppq_ = 96;
  bool playing_ = false, jumped_ = false;
  int64_t blockOff_ = 0;
  double vol_ = 0.8, panL_ = 1, panR_ = 1;
  int base_ = 60, keyMap_ = 0, sync_ = SyncStretch;
  bool lock_ = true, mono_ = false, reverseAll_ = false;
  double attack_ = 0, decay_ = 0, sustain_ = 1, release_ = 0.01, declick_ = 0.003;
  double swing_ = 0, pitch_ = 0, velSens_ = 0.6, gate_ = 1, speed_ = 1;
  bool filterOn_ = false;
  double g_ = 0, k_ = 2, reso_ = 0;
  double ic1L_ = 0, ic2L_ = 0, ic1R_ = 0, ic2R_ = 0;
  // pattern
  bool patOn_ = false, patPrimed_ = false;
  uint32_t patId_ = 0;
  double patSemis_ = 0;
  float patVel_ = 1;
  double patElapsed_ = 0, lastP_ = 0;
  int nextStep_ = 0, nextSub_ = 0;
  std::atomic<int> display_{-1};
  std::atomic<double> liveFrame_{-1}, livePat_{-1};
  std::atomic<bool> patRun_{false};  // J-110: patOn_ for the editor's Play button

  // The pattern starts / stops (from step 1 on its next start: noteOnImpl).
  void setPat(bool on) {
    patOn_ = on;
    patRun_.store(on, std::memory_order_relaxed);
    if (!on) livePat_.store(-1, std::memory_order_relaxed);
  }

  // UI side channel: the source frame under the newest voice, the pattern position.
  void publish() {
    const Voice* nv = nullptr;
    for (const auto& v : voices_)
      if (v.on && (!nv || v.age > nv->age)) nv = &v;
    double f = -1;
    if (nv && aux_) {
      if (nv->warped && aux_->warp && render_) f = frameAt(aux_->grid, aux_->warp->beatA + nv->pos / (60.0 / render_->tempo * render_->rate));
      else f = nv->pos;
    }
    liveFrame_.store(f, std::memory_order_relaxed);
    double pp = -1;
    if (patOn_ && aux_ && aux_->patUnits > 0) pp = lastP_ / (aux_->patUnits * aux_->unitBeats);
    livePat_.store(pp, std::memory_order_relaxed);
  }
};

// ================================================================== offline helpers
PluginState stateFor(const std::vector<float>& params) {
  PluginState st = defaultPluginState(kType);
  for (size_t i = 0; i < params.size() && i < st.params.size(); ++i) st.params[i] = params[i];
  return st;
}

struct Offline {
  std::shared_ptr<SeltmannAux> aux;
  std::unique_ptr<Render> warp;
  ChannelAux ch;
  PluginState st;
  SeltmannGen gen;
};

std::unique_ptr<Offline> makeOffline(const Model& m, SamplePtr src, const RenderOpts& o) {
  if (!src || m.slices.empty()) return nullptr;
  auto off = std::make_unique<Offline>();
  off->st = stateFor(o.params);
  off->st.params[SmSync] = static_cast<float>(o.sync);
  off->aux = buildAux(m, src, "", o.stretchMode, false);
  if (o.sync == SyncStretch && off->aux->warp) {
    off->warp = makeRender(*off->aux->warp, o.tempo);
    off->gen.forceRender(off->warp.get());
  }
  off->ch.custom = off->aux;
  off->ch.samples.assign(1, src);
  return off;
}

void run(Offline& off, const RenderOpts& o, double startTick, int64_t frames, std::vector<float>& L, std::vector<float>& R,
         const std::function<void(Generator&, int64_t)>& events) {
  L.assign(static_cast<size_t>(frames), 0.f);
  R.assign(static_cast<size_t>(frames), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = o.sampleRate;
  ctx.tempo = o.tempo;
  ctx.ppq = 960;
  ctx.playing = true;
  ctx.offline = true;
  const int block = 256;
  for (int64_t pos = 0; pos < frames; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
    ctx.tick = startTick + static_cast<double>(pos) / o.sampleRate * o.tempo / 60.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    if (events) events(off.gen, pos);
    off.gen.process(off.st, off.ch, ctx, L.data() + pos, R.data() + pos, n);
  }
}

}  // namespace

bool liveView(const Generator* g, LiveView& out) {
  const auto* sg = dynamic_cast<const SeltmannGen*>(g);
  if (!sg) return false;
  const int d = sg->displayStep();
  out.step = displayStepOf(d);
  out.slice = displaySliceOf(d);
  out.srcFrame = sg->liveFrame();
  out.patternPos = sg->livePattern();
  out.patternRunning = sg->patternRunning();
  return true;
}

// ================================================================== pack entry points

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == kType) return std::make_unique<SeltmannGen>();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  const std::string path = pathOf(state.data);
  aux.samples.clear();
  aux.custom.reset();
  if (path.empty()) return true;
  SamplePtr src = sampleCache().get(path);
  aux.samples.assign(1, src);
  if (!src) return true;
  (void)stretch::sincTable(stretch::Quality::Realtime);
  Model m;
  std::string p2;
  if (!modelOf(state, src, p2, m)) return true;
  const int mode = std::clamp(static_cast<int>(std::lround(state.params.size() > SmStretchMode ? state.params[SmStretchMode] : 0)), 0, 2);
  aux.custom = buildAux(m, src, path, mode, true);
  return true;
}

// ================================================================== renders

double patternSeconds(const Model& m, const RenderOpts& o) {
  const int sp = o.params.size() > SmSpeed ? std::clamp(static_cast<int>(std::lround(o.params[SmSpeed])), 0, 2) : SpeedNormal;
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  const double bpm = o.sync == SyncOriginal ? (m.grid.bpm > 1 ? m.grid.bpm : 120.0) : o.tempo;
  return m.pattern.beats() / speed * 60.0 / bpm;
}

bool renderPatternLoop(const Model& m, SamplePtr src, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  auto off = makeOffline(m, src, o);
  if (!off || m.pattern.steps.empty()) return false;
  const int base = std::clamp(static_cast<int>(std::lround(off->st.params[SmBaseNote])), 12, 127);
  const int64_t cycle = static_cast<int64_t>(std::llround(patternSeconds(m, o) * o.sampleRate));
  if (cycle <= 0) return false;
  // free-running (no song lock) from the note: three cycles, keep the middle one,
  // which carries the previous cycle's tails exactly as the loop wraps
  off->st.params[SmLock] = 0;
  std::vector<float> l3, r3;
  run(*off, o, 0, cycle * 3, l3, r3, [&](Generator& g, int64_t pos) {
    if (pos == 0) g.noteOn(1, base - 12, 1.0f, 0);
  });
  L.assign(l3.begin() + cycle, l3.begin() + 2 * cycle);
  R.assign(r3.begin() + cycle, r3.begin() + 2 * cycle);
  return true;
}

bool renderSlice(const Model& m, SamplePtr src, int slice, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  auto off = makeOffline(m, src, o);
  if (!off || slice < 0 || slice >= static_cast<int>(m.slices.size())) return false;
  off->st.params[SmKeyMap] = KeySlices;
  const int base = std::clamp(static_cast<int>(std::lround(off->st.params[SmBaseNote])), 0, 127);
  const auto& sl = m.slices[static_cast<size_t>(slice)];
  const double sp = o.sync == SyncOriginal ? 1.0 : o.tempo / std::max(1.0, m.grid.bpm);
  const double pitch = std::pow(2.0, (sl.pitch + (o.params.size() > SmPitch ? o.params[SmPitch] : 0.0f)) / 12.0);
  const double secs = static_cast<double>(sl.end - sl.start) / src->sampleRate / std::max(0.05, sp * pitch);
  const int64_t frames = static_cast<int64_t>(std::llround(secs * o.sampleRate)) + static_cast<int64_t>(o.sampleRate * 0.01);
  if (base + slice > 127) return false;
  run(*off, o, 0, frames, L, R, [&](Generator& g, int64_t pos) {
    if (pos == 0) g.noteOn(1, base + slice, 1.0f, 0);
  });
  // trim trailing silence
  int64_t end = static_cast<int64_t>(L.size());
  while (end > 1 && std::fabs(L[static_cast<size_t>(end - 1)]) < 1e-7f && std::fabs(R[static_cast<size_t>(end - 1)]) < 1e-7f) --end;
  L.resize(static_cast<size_t>(end));
  R.resize(static_cast<size_t>(end));
  return true;
}

bool renderSpan(const Model& m, SamplePtr src, double b0, double b1, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  if (!src || b1 <= b0 || o.tempo <= 0) return false;
  const double fade = 0.005;  // seam crossfade (s)
  const double bpm = o.sync == SyncOriginal ? std::max(1.0, m.grid.bpm) : o.tempo;
  const int64_t len = static_cast<int64_t>(std::llround((b1 - b0) * 60.0 / bpm * o.sampleRate));
  const int64_t F = std::max<int64_t>(8, static_cast<int64_t>(fade * o.sampleRate));
  const double preBeats = static_cast<double>(F) / o.sampleRate * bpm / 60.0;
  const double a0 = b0 - preBeats;
  std::vector<float> l, r;
  const int64_t total = len + F;
  l.assign(static_cast<size_t>(total), 0.f);
  r.assign(static_cast<size_t>(total), 0.f);
  const Sample& s = *src;
  const int64_t n = static_cast<int64_t>(s.frames());
  const float* in[2] = {s.left.data(), s.right.data()};
  if (o.sync == SyncStretch) {
    WarpSlot w;
    w.src = src;
    w.grid = m.grid;
    w.a = std::clamp<int64_t>(static_cast<int64_t>(std::floor(frameAt(m.grid, a0))) - 1, 0, n - 1);
    w.b = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(frameAt(m.grid, b1))) + 1, w.a + 1, n);
    float* out[2] = {l.data(), r.data()};
    const float* inw[2] = {in[0] + w.a, in[1] + w.a};
    stretch::Settings st;
    st.mode = modeOf(o.stretchMode);
    st.pitch = s.sampleRate / o.sampleRate;  // output frames play at the project rate: keep the musical pitch
    const double outBeat = 60.0 / bpm * o.sampleRate;
    stretch::processMapped(inw, 2, w.b - w.a, out, total, s.sampleRate, st, [&](int64_t j) {
      return std::clamp(frameAt(m.grid, a0 + static_cast<double>(j) / outBeat) - static_cast<double>(w.a), 0.0, static_cast<double>(w.b - w.a - 1));
    });
  } else {
    // resample: read the source along the grid (speed follows the project tempo)
    const auto& tab = stretch::sincTable(stretch::Quality::Offline);
    const double outBeat = 60.0 / bpm * o.sampleRate;
    for (int64_t j = 0; j < total; ++j) {
      const double f = frameAt(m.grid, a0 + static_cast<double>(j) / outBeat);
      const double f2 = frameAt(m.grid, a0 + static_cast<double>(j + 1) / outBeat);
      float x, y;
      stretch::readSinc2(in[0], in[1], n, f, std::max(0.01, f2 - f), tab, x, y);
      l[static_cast<size_t>(j)] = x;
      r[static_cast<size_t>(j)] = y;
    }
  }
  // the loop: [F, F + len); its last F frames fade into the audio that preceded b0
  L.assign(l.begin() + F, l.end());
  R.assign(r.begin() + F, r.end());
  for (int64_t k = 0; k < F && k < len; ++k) {
    const double w = (static_cast<double>(k) + 0.5) / static_cast<double>(F);
    const double c = 0.5 - 0.5 * std::cos(w * 3.14159265358979);
    const size_t d = static_cast<size_t>(len - F + k);
    L[d] = static_cast<float>(L[d] * (1 - c) + l[static_cast<size_t>(k)] * c);
    R[d] = static_cast<float>(R[d] * (1 - c) + r[static_cast<size_t>(k)] * c);
  }
  return true;
}

std::vector<PatNote> patternNotes(const Model& m, int baseNote, float swing, float gate) {
  std::vector<PatNote> v;
  const int n = static_cast<int>(m.slices.size());
  for (int i = 0; i < static_cast<int>(m.pattern.steps.size()); ++i) {
    const Step& st = m.pattern.steps[static_cast<size_t>(i)];
    const int k = stepSlice(m.pattern, st, n);
    if (k < 0 || baseNote + k > 127) continue;
    const double t0 = stepStart(m.pattern, i, swing);
    const double t1 = i + 1 < static_cast<int>(m.pattern.steps.size()) ? stepStart(m.pattern, i + 1, swing) : m.pattern.beats();
    const int rep = std::clamp(st.stutter, 1, 16);
    const double sub = (t1 - t0) / rep;
    for (int r = 0; r < rep; ++r) {
      PatNote pn;
      pn.start = t0 + r * sub;
      pn.length = std::max(1.0 / 96, sub * std::clamp(static_cast<double>(gate), 0.05, 1.0) * st.length);
      pn.key = baseNote + k;
      pn.velocity = std::clamp(static_cast<int>(std::lround(100 * dbToGain(st.gain))), 1, 127);
      pn.modX = std::clamp(128 + static_cast<int>(std::lround(st.pitch * 256.0 / 48.0)), 0, 255);
      pn.modY = st.reverse ? 255 : 128;
      v.push_back(pn);
    }
  }
  return v;
}

bool writeLoopWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, double rate) {
  AudioBuffer b(2, L.size());
  for (size_t i = 0; i < L.size(); ++i) {
    b.at(0, i) = L[i];
    b.at(1, i) = i < R.size() ? R[i] : L[i];
  }
  return writeWav(path, b, static_cast<int>(std::lround(rate)), 24);
}

bool writePatternMidi(const std::string& path, const std::vector<PatNote>& notes, double tempo) {
  MidiFileData d;
  d.division = 96;
  d.tempo = tempo;
  MidiTrack t;
  t.name = "Seltmann";
  for (const auto& n : notes) {
    MidiNote mn;
    mn.start = static_cast<int64_t>(std::llround(n.start * 96));
    mn.length = std::max<int64_t>(1, static_cast<int64_t>(std::llround(n.length * 96)));
    mn.pitch = n.key;
    mn.velocity = n.velocity;
    t.notes.push_back(mn);
  }
  d.tracks.push_back(t);
  return writeMidiFile(path, d);
}

double patternBars(const Model& m, const std::vector<float>& params, int beatsPerBar) {
  const int sp = params.size() > SmSpeed ? std::clamp(static_cast<int>(std::lround(params[SmSpeed])), 0, 2) : SpeedNormal;
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  return m.pattern.beats() / speed / std::max(1, beatsPerBar);
}

ObjectId sendPatternToSong(Song& s, ObjectId channel, const Model& m, const std::vector<float>& params, int cycles, int track, Tick at) {
  if (m.pattern.steps.empty() || !s.channel(channel)) return kNoId;
  auto prm = [&](int k, float d) { return k < static_cast<int>(params.size()) ? params[static_cast<size_t>(k)] : d; };
  const int base = std::clamp(static_cast<int>(std::lround(prm(SmBaseNote, 60))), 0, 127);
  const int sp = std::clamp(static_cast<int>(std::lround(prm(SmSpeed, SpeedNormal))), 0, 2);
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  const auto notes = patternNotes(m, base, prm(SmSwing, 0), prm(SmGate, 1));
  const double cycleBeats = m.pattern.beats() / speed;
  cycles = std::clamp(cycles, 1, 64);
  gl::Pattern p;
  p.id = s.allocateId();
  p.name = "Seltmann " + m.pattern.name;
  p.color = 0xE0A04A;
  const double beatsPerBar = std::max<double>(1, static_cast<double>(s.barTicks()) / std::max(1, s.ppq));
  p.bars = std::clamp(static_cast<int>(std::ceil(cycleBeats * cycles / beatsPerBar - 1e-9)), 1, kMaxPatternBars);
  auto& row = p.notes[channel];
  for (int c = 0; c < cycles; ++c)
    for (const auto& n : notes) {
      Note nn;
      nn.start = static_cast<Tick>(std::llround((c * cycleBeats + n.start / speed) * s.ppq));
      nn.length = std::max<Tick>(1, static_cast<Tick>(std::llround(n.length / speed * s.ppq)));
      nn.pitch = n.key;
      nn.velocity = n.velocity;
      nn.modX = n.modX;
      nn.modY = n.modY;
      row.push_back(nn);
    }
  s.patterns.push_back(p);
  if (track >= 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = p.id;
    c.track = std::max(0, track);
    c.start = std::max<Tick>(0, at);
    c.length = static_cast<Tick>(std::llround(cycleBeats * cycles * s.ppq));
    s.playlist.push_back(c);
  }
  return p.id;
}

}  // namespace gl::packs::seltmann

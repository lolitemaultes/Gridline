// Gridline — "platter" pack: the deck runtime (gl.platter).
//
// Everything advances in control steps of kStep output samples (shorter when an
// event cuts a step), whatever the host block size, so the deck renders the
// same audio for any block size and a recorded take replays sample for sample.
// Per step the deck decides where the read head ends (hand tracking, motor,
// loops, jumps) and the level; inside the step the head moves at a constant
// speed, every sample read through Gridline's band-limited kernel at that
// speed (any direction, down to zero), so a direction change is a continuous
// stop and turn, never a click. Jumps crossfade from a ghost reader that keeps
// the old motion for a few milliseconds.
//
// Physics:
//   hand    the platter follows the hand through an alpha-beta tracker (a
//           critically damped second-order loop; Hand feel sets its
//           bandwidth), which turns coarse jog ticks or mouse moves into a
//           smooth position and speed;
//   motor   playing pulls the platter to its speed at a torque set by Start
//           time, stopping brakes it at Stop time; a throw keeps its speed and
//           the motor catches it;
//   nudge   rim turns (and the platter top in CDJ mode) bend the speed for a
//           moment; while stopped they move the record by hand.
//
// Audio thread: no allocation, no locks, no I/O. Inputs: the controller hub
// (device MIDI, app), raw channel MIDI (plugin builds), notes, and editor
// commands through a single-producer ring.
#include "../Packs.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../../engine/TimeStretch.h"
#include "../../engine/pitch/Kernel.h"
#include "platter.h"

namespace gl::packs::platter {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFade = 128;  // jump / path crossfade (samples)

float par(const PluginState& s, int i, float def) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : def; }
float defOf(int i) {
  static std::array<float, PCount> d = [] {
    std::array<float, PCount> x{};
    if (const PluginInfo* p = findPlugin(kType))
      for (int k = 0; k < PCount && k < static_cast<int>(p->params.size()); ++k) x[static_cast<size_t>(k)] = static_cast<float>(p->params[static_cast<size_t>(k)].def);
    return x;
  }();
  return d[static_cast<size_t>(i)];
}
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ================================================================== aux
struct PlatterAux final : AuxData {
  SamplePtr src;
  Model m;
  uint32_t device = 0;  // hub id of the bound device (0 = any)
};

std::shared_ptr<const PlatterAux> cachedAux(const std::string& data, const SamplePtr& src) {
  static std::mutex mu;
  static std::map<std::string, std::weak_ptr<const PlatterAux>> cache;
  std::lock_guard<std::mutex> lk(mu);
  for (auto it = cache.begin(); it != cache.end();) it = it->second.expired() ? cache.erase(it) : std::next(it);
  const std::string key = data + "|" + std::to_string(reinterpret_cast<uintptr_t>(src.get()));
  if (auto it = cache.find(key); it != cache.end())
    if (auto p = it->second.lock()) return p;
  auto x = std::make_shared<PlatterAux>();
  x->src = src;
  std::string path;
  Model m;
  const bool has = parseData(data, path, m) && src && m.srcFrames == static_cast<int64_t>(src->frames());
  if (!has) {
    // a bare path (or a changed file): a flat grid until the editor stores the analysis
    Model keep = m;
    if (src) m = carryOver(keep, defaultModel(*src, seltmann::flatGrid(*src, 120), false));
  }
  x->m = std::move(m);
  x->device = hub::deviceId(x->m.map.device);
  cache[key] = x;
  return x;
}

// ================================================================== DSP pieces
// TPT state-variable filter (one channel).
struct Svf {
  double ic1 = 0, ic2 = 0;
  void reset() { ic1 = ic2 = 0; }
  // returns lp, bp, hp through refs
  inline void run(double x, double g, double k, double& lp, double& bp, double& hp) {
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + g * a1 * v3;
    const double v2 = ic2 + g * v1;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
    lp = v2, bp = v1, hp = x - k * v1 - v2;
  }
};

// DJ isolator EQ: low = LR4 low-pass at 300 Hz, high = LR4 high-pass at 2.8 kHz
// (each two cascaded Butterworth sections), mid = what is left, so the three
// bands always sum back to the input exactly.
struct Isolator {
  Svf lo[2][2], hi[2][2];
  double gLo = 0, gHi = 0;
  void setup(double sr) {
    gLo = std::tan(kPi * 300.0 / sr);
    gHi = std::tan(kPi * std::min(2800.0, sr * 0.45) / sr);
  }
  void reset() {
    for (auto& c : lo)
      for (auto& s : c) s.reset();
    for (auto& c : hi)
      for (auto& s : c) s.reset();
  }
  inline void bands(int ch, double x, double& l, double& m, double& h) {
    const double k = 1.41421356237;  // Butterworth
    double a, b, c;
    lo[ch][0].run(x, gLo, k, a, b, c);
    double l2;
    lo[ch][1].run(a, gLo, k, l2, b, c);
    double h1, h2;
    hi[ch][0].run(x, gHi, k, a, b, h1);
    hi[ch][1].run(h1, gHi, k, a, b, h2);
    l = l2, h = h2, m = x - l2 - h2;
  }
};

// ================================================================== key lock voices
struct Reader {
  const Sample* src = nullptr;
  int64_t cursor = 0;
};
int readFrames(void* ctx, float* const* dst, int frames, bool) {
  auto* r = static_cast<Reader*>(ctx);
  const int64_t n = r->src ? static_cast<int64_t>(r->src->frames()) : 0;
  for (int i = 0; i < frames; ++i) {
    const int64_t k = r->cursor + i;
    const bool in = k >= 0 && k < n;
    dst[0][i] = in ? r->src->left[static_cast<size_t>(k)] : 0.f;
    dst[1][i] = in ? r->src->right[static_cast<size_t>(k)] : 0.f;
  }
  r->cursor += frames;
  return -1;
}
struct KeyPool {
  double rate = 0;
  stretch::Voice v[2];
  Reader rd[2];
};

// ================================================================== the deck
class PlatterGen final : public Generator {
 public:
  PlatterGen() {
    (void)pitch::table(pitch::Quality::Realtime);
    capture_.resize(kCapture);
    decoderRaw_.reset();
    decoderHub_.reset();
    hubPos_ = hub::head();
    for (auto& a : ctlUntil_) a = 0;
    for (auto& v : ctlVal_) v = 0;
    for (int i = 0; i < PCount; ++i) p_[static_cast<size_t>(i)] = defOf(i), lastState_[static_cast<size_t>(i)] = defOf(i);
    hot_.fill(-1);
    for (auto& h : pubHot_) h.store(-1);
  }
  ~PlatterGen() override {
    delete pool_;
    delete pending_.load();
    delete retired_.load();
  }

  bool active() const override { return loaded_ && (motor_ || hand_ || std::fabs(vel_) > 1e-9 || ghost_.left > 0 || takeOn_); }
  bool ownPlayback() const override { return motor_ && !transport_; }
  void stopOwnPlayback() override { motor_ = false, cuePreview_ = false, hotPreview_ = -1, playLatched_ = false; }
  bool rawMidi() const override { return true; }

  // UI thread (Program build): the key lock voices for this sample rate.
  void prepareHost(const PluginState& state, double sampleRate) override {
    (void)state;
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    if (sampleRate == uiRate_) return;
    auto* p = new KeyPool;
    p->rate = sampleRate;
    for (auto& v : p->v) v.prepare(2, sampleRate, stretch::Mode::Auto, 2.5, 2.5);
    delete pending_.exchange(p, std::memory_order_acq_rel);
    uiRate_ = sampleRate;
  }

  // ---- UI side (thread-safe)
  bool pushCmd(const Cmd& c) {
    const size_t h = cmdHead_.load(std::memory_order_relaxed);
    if (h - cmdTail_.load(std::memory_order_acquire) >= cmds_.size()) return false;
    cmds_[h % cmds_.size()] = c;
    cmdHead_.store(h + 1, std::memory_order_release);
    return true;
  }
  void view(LiveView& v) const {
    v.valid = true;
    v.loaded = pubLoaded_.load(std::memory_order_relaxed);
    v.pos = pubPos_.load(std::memory_order_relaxed);
    v.speed = pubSpeed_.load(std::memory_order_relaxed);
    v.playing = pubMotor_.load(std::memory_order_relaxed);
    v.touched = pubHand_.load(std::memory_order_relaxed);
    v.slipping = pubSlipping_.load(std::memory_order_relaxed);
    v.slipPos = pubSlipPos_.load(std::memory_order_relaxed);
    v.loopOn = pubLoopOn_.load(std::memory_order_relaxed);
    v.loopIn = pubLoopIn_.load(std::memory_order_relaxed);
    v.loopOut = pubLoopOut_.load(std::memory_order_relaxed);
    v.cue = pubCue_.load(std::memory_order_relaxed);
    v.cueLit = pubCueLit_.load(std::memory_order_relaxed);
    for (int k = 0; k < kHotCues; ++k) v.hot[static_cast<size_t>(k)] = pubHot_[static_cast<size_t>(k)].load(std::memory_order_relaxed);
    const uint32_t f = pubFlags_.load(std::memory_order_relaxed);
    v.sync = f & 1, v.keylock = f & 2, v.slip = f & 4, v.vinyl = f & 8, v.quantize = f & 16, v.keylockActive = f & 32, v.shift = f & 64;
    v.capturing = f & 128;
    v.tempo = pubTempo_.load(std::memory_order_relaxed);
    v.take = pubTake_.load(std::memory_order_relaxed);
    v.level = pubLevel_.load(std::memory_order_relaxed);
    v.rawSeq = rawSeq_.load(std::memory_order_acquire);
    const uint32_t r = raw_.load(std::memory_order_relaxed);
    v.raw[0] = static_cast<uint8_t>(r & 0xFF), v.raw[1] = static_cast<uint8_t>((r >> 8) & 0xFF), v.raw[2] = static_cast<uint8_t>((r >> 16) & 0xFF);
  }
  void mirrorOut(Mirror& m) const {
    for (int i = 0; i < PCount; ++i) {
      m.seq[static_cast<size_t>(i)] = mirSeq_[static_cast<size_t>(i)].load(std::memory_order_acquire);
      m.value[static_cast<size_t>(i)] = mirVal_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
  }
  void marksOut(Marks& m) const {
    m.seq = marksSeq_.load(std::memory_order_acquire);
    m.cue = pubCue_.load(std::memory_order_relaxed);
    for (int k = 0; k < kHotCues; ++k) m.hot[static_cast<size_t>(k)] = pubHot_[static_cast<size_t>(k)].load(std::memory_order_relaxed);
    m.loopIn = pubSavedIn_.load(std::memory_order_relaxed);
    m.loopOut = pubSavedOut_.load(std::memory_order_relaxed);
  }
  int drain(CaptureEvent* out, int max) {
    int n = 0;
    size_t t = capTail_.load(std::memory_order_relaxed);
    const size_t h = capHead_.load(std::memory_order_acquire);
    while (t != h && n < max) out[n++] = capture_[t++ % capture_.size()];
    capTail_.store(t, std::memory_order_release);
    return n;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float) override {
    const int k = static_cast<int>(std::lround(pitch));
    for (auto& h : held_)
      if (h.key < 0) {
        h.id = id;
        h.key = k;
        break;
      }
    key(k, true);
  }
  void onNoteOff(uint32_t id) override {
    // the pitch is not given on release: the key this id started
    for (auto& h : held_)
      if (h.id == id && h.key >= 0) {
        const int k = h.key;
        h.key = -1;
        key(k, false);
      }
  }
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps&) override { onNoteOn(id, pitch, velocity); }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const int kind = status & 0xF0;
    noteRaw(status, d1, d2);
    if (hub::tapped()) {
      // the app reads devices from the hub: channel MIDI only plays keys (not
      // the deck's own controller buttons)
      if ((kind == 0x90 || kind == 0x80) && !boundNote(status, d1)) key(d1, kind == 0x90 && d2 > 0);
      return;
    }
    // plugin builds: the host's MIDI is the controller
    InEvent ev[16];
    const int n = aux_ ? decoderRaw_.feed(aux_->m.map, status, d1, d2, ev, 16) : 0;
    for (int i = 0; i < n; ++i) input(ev[i]);
    if (n == 0 && (kind == 0x90 || kind == 0x80)) key(d1, kind == 0x90 && d2 > 0);
  }
  void onAllOff() override {
    for (auto& h : held_) h.key = -1;
    if (noteHot_ >= 0) key(kHotKeyFirst + noteHot_, false);
    if (takeOn_) stopTake();
  }
  void onKill() override {
    motor_ = false, hand_ = false, softHand_ = false, vel_ = 0, cuePreview_ = false, hotPreview_ = -1, playLatched_ = false, noteHot_ = -1;
    censor_ = false, bend_ = 0, nudge_ = 0;
    ghost_.left = 0;
    if (takeOn_) stopTake();
    for (auto& h : held_) h.key = -1;
    stepPos_ = stepLen_ = 0;
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    // key lock voices from the UI thread
    if (KeyPool* np = pending_.load(std::memory_order_acquire); np && !retired_.load(std::memory_order_relaxed)) {
      pending_.store(nullptr, std::memory_order_relaxed);
      retired_.store(pool_, std::memory_order_release);
      pool_ = np;
      keyOn_ = false;
      keyGhost_ = -1;
    }
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    if (sr_ != eqRate_) {
      eq_.setup(sr_);
      eqRate_ = sr_;
    }
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120.0;
    ppq_ = std::max(1, ctx.ppq);
    blockTick_ = ctx.tick;
    blockClock_ = clock_;
    transport_ = ctx.playing;
    const bool wasRecording = recording_;
    recording_ = ctx.recording && ctx.playing && !ctx.offline;
    offline_ = ctx.offline;
    // aux (model): a new one adopts its cues unless the deck set newer ones
    const auto* nx = dynamic_cast<const PlatterAux*>(aux.custom.get());
    if (nx != aux_) {
      const bool fileChanged = !aux_ || !nx || aux_->src != nx->src;
      aux_ = nx;
      if (aux_) adoptMarks(fileChanged);
      if (fileChanged) {
        motor_ = false, vel_ = 0, hand_ = false, softHand_ = false, ghost_.left = 0, loopOn_ = false;
        if (takeOn_) stopTake();
        pos_ = aux_ ? cue_ : 0;
        keyOn_ = false;
        decoderHub_.reset();
        decoderRaw_.reset();
      }
    }
    loaded_ = aux_ && aux_->src && aux_->src->frames() > 0;
    // parameters: the controller's last value wins until the parameter itself changes
    for (int i = 0; i < PCount; ++i) {
      const auto k = static_cast<size_t>(i);
      const float v = par(s, i, defOf(i));
      if (v != lastState_[k]) {
        lastState_[k] = v;
        if (clock_ >= ctlUntil_[k] || v == ctlVal_[k]) ctlUntil_[k] = 0;  // automation / an edit; or the editor mirrored us
      }
      p_[k] = ctlUntil_[k] > 0 ? ctlVal_[k] : v;
    }
    // controller MIDI from the hub (app): this deck's device
    if (hub::tapped()) {
      const uint64_t head = hub::head();
      if (head - hubPos_ > hub::kSize) hubPos_ = head - hub::kSize;
      const uint32_t want = aux_ ? aux_->device : 0;
      InEvent ev[16];
      for (; hubPos_ < head; ++hubPos_) {
        uint32_t dev = 0;
        uint8_t st = 0, a = 0, b = 0;
        if (!hub::read(hubPos_, dev, st, a, b)) continue;
        if (want != 0 && dev != want) continue;
        noteRaw(st, a, b);
        if (!aux_) continue;
        const int n = decoderHub_.feed(aux_->m.map, st, a, b, ev, 16);
        for (int i = 0; i < n; ++i) input(ev[i]);
      }
    } else {
      hubPos_ = hub::head();
    }
    // capture: begins on the next beat while Gridline records, ends when it stops or the position jumps
    if (capturing_ && (!recording_ || ctx.jumped || !armed())) endCapture();
    if (recording_ && armed() && loaded_ && !capturing_ && capStartAt_ < 0) {
      const double spt = sr_ * 60.0 / (tempo_ * ppq_);  // samples per tick
      const double beat = static_cast<double>(ppq_);
      double tb = std::ceil(blockTick_ / beat - 1e-9) * beat;
      const double off = (tb - blockTick_) * spt;
      capStartAt_ = static_cast<int64_t>(clock_) + static_cast<int64_t>(std::ceil(off - 1e-9));
      capStartTick_ = tb;
    }
    if (!recording_) capStartAt_ = -1;
    (void)wasRecording;
    // editor commands
    for (size_t t = cmdTail_.load(std::memory_order_relaxed); t != cmdHead_.load(std::memory_order_acquire); ++t) {
      command(cmds_[t % cmds_.size()]);
      cmdTail_.store(t + 1, std::memory_order_release);
    }
  }

  void render(float* L, float* R, int n) override {
    int i = 0;
    while (i < n) {
      if (capStartAt_ >= 0 && !capturing_) {
        if (static_cast<int64_t>(clock_) >= capStartAt_) {
          cut();  // the take starts exactly here, on a fresh step
          beginCapture();
        }
      }
      if (stepPos_ >= stepLen_) computeStep();
      int run = std::min(n - i, stepLen_ - stepPos_);
      if (capStartAt_ >= 0 && !capturing_ && static_cast<int64_t>(clock_) < capStartAt_)
        run = static_cast<int>(std::min<int64_t>(run, capStartAt_ - static_cast<int64_t>(clock_)));
      runStep(L + i, R + i, run);
      i += run;
    }
    publish();
  }
  bool handlesNotePan() const override { return true; }

 private:
  // ---------------------------------------------------------------- helpers
  bool armed() const { return aux_ && aux_->m.armed; }
  bool on(int p) const { return p_[static_cast<size_t>(p)] >= 0.5f; }
  int ipar(int p) const { return static_cast<int>(std::lround(p_[static_cast<size_t>(p)])); }
  double srcRate() const { return loaded_ ? aux_->src->sampleRate : sr_; }
  double frames() const { return loaded_ ? static_cast<double>(aux_->src->frames()) : 0.0; }
  double unity() const { return srcRate() / sr_; }  // read speed of the original tempo (frames per output sample)
  const Model& model() const { return aux_->m; }

  // Speed of the motor (frames per output sample) at the current position.
  double baseSpeed() const {
    if (!loaded_) return 0;
    if (on(PSync)) {
      const double b = beatAt(model(), pos_);
      const double fb = std::floor(b);
      double len = frameAt(model(), fb + 1) - frameAt(model(), fb);
      if (!(len > 1)) len = 60.0 * srcRate() / bpmOf(model());
      return len / (sr_ * 60.0 / tempo_);
    }
    return unity() * (1.0 + p_[PPitch] * pitchRangeOf(ipar(PPitchRange)));
  }
  double effectiveBpm() const {
    if (!loaded_) return 0;
    if (on(PSync)) return tempo_;
    return bpmOf(model()) * (1.0 + p_[PPitch] * pitchRangeOf(ipar(PPitchRange)));
  }
  double quantized(double f) const { return on(PQuantize) && loaded_ ? nearestBeatFrame(model(), f) : f; }
  // A jump target that keeps the beat phase while playing (quantize).
  double phaseJump(double target) const {
    if (!on(PQuantize) || !motor_ || !loaded_) return target;
    const double bNow = beatAt(model(), pos_), bT = beatAt(model(), target);
    const double ph = bNow - std::floor(bNow);
    return frameAt(model(), std::round(bT) + ph);
  }
  double songBeat(double sampleInBlock) const {
    return (blockTick_ + sampleInBlock * tempo_ * ppq_ / (60.0 * sr_)) / static_cast<double>(ppq_);
  }

  void adoptMarks(bool fileChanged) {
    const Model& m = aux_->m;
    const bool same = cue_ == m.cue && hot_ == m.hot && savedIn_ == m.loopIn && savedOut_ == m.loopOut;
    if (marksDirty_ && !fileChanged) {
      if (same) marksDirty_ = false;  // the editor stored what the deck set
      return;
    }
    marksDirty_ = false;
    cue_ = m.cue, hot_ = m.hot, savedIn_ = m.loopIn, savedOut_ = m.loopOut;
  }
  void marksChanged() {
    marksDirty_ = true;
    marksSeq_.fetch_add(1, std::memory_order_release);
  }

  bool boundNote(uint8_t s, uint8_t d1) const {
    if (!aux_) return false;
    const int ch = s & 0x0F;
    for (const Binding& b : aux_->m.map.bindings)
      if (b.kind == MkNote && b.num == d1 && (b.channel < 0 || b.channel == ch)) return true;
    return false;
  }
  void noteRaw(uint8_t s, uint8_t a, uint8_t b) {
    raw_.store(static_cast<uint32_t>(s) | (static_cast<uint32_t>(a) << 8) | (static_cast<uint32_t>(b) << 16), std::memory_order_relaxed);
    rawSeq_.fetch_add(1, std::memory_order_release);
  }

  void setCtl(int p, float v) {
    const auto k = static_cast<size_t>(p);
    ctlVal_[k] = v;
    ctlUntil_[k] = clock_ + static_cast<uint64_t>(sr_ * 0.25) + 1;
    p_[k] = v;
    mirVal_[k].store(v, std::memory_order_relaxed);
    mirSeq_[k].fetch_add(1, std::memory_order_release);
  }
  void toggleCtl(int p) { setCtl(p, on(p) ? 0.f : 1.f); }

  // ---------------------------------------------------------------- transport actions
  void jump(double target) {
    if (!loaded_) return;
    target = std::clamp(target, -srcRate() * 2, frames() + srcRate() * 2);
    jumpTo_ = target;
    jumpPending_ = true;
    if (hand_) handPos_ = target, trX_ = target;
  }
  void startMotor(bool instant) {
    if (!motor_ && on(PSync) && on(PQuantize) && transport_ && loaded_ && !hand_) {
      // land in phase with the project's beat
      const double sb = songBeat(static_cast<double>(stepClock() - blockClock_));
      const double db = beatAt(model(), jumpPending_ ? jumpTo_ : pos_);
      const double want = sb - std::floor(sb);
      double tgt = std::floor(db) + want;
      if (tgt - db > 0.5) tgt -= 1;
      if (db - tgt > 0.5) tgt += 1;
      jump(frameAt(model(), tgt));
    }
    motor_ = true;
    if (instant) vel_ = baseSpeed();
  }
  void stopMotor(bool instant) {
    motor_ = false;
    if (instant) vel_ = 0;
  }
  uint64_t stepClock() const { return clock_; }

  void buttonPlay(bool down) {
    if (!down) return;
    if (cuePreview_ || hotPreview_ >= 0) {
      playLatched_ = true;
      return;
    }
    if (motor_) stopMotor(p_[PStopTime] <= 0.001f);
    else startMotor(p_[PStartTime] <= 0.001f);
  }
  void buttonCue(bool down) {
    if (!loaded_) return;
    if (down) {
      if (hand_) {
        cue_ = quantized(pos_);
        marksChanged();
      } else if (motor_ && !cuePreview_) {
        jump(cue_);
        stopMotor(true);
      } else if (!motor_) {
        if (std::fabs(pos_ - cue_) < 0.5 * srcRate() * 0.01 + 1) {
          cuePreview_ = true;
          playLatched_ = false;
          startMotor(true);
        } else {
          cue_ = quantized(pos_);
          marksChanged();
          if (cue_ != pos_) jump(cue_);
        }
      }
    } else if (cuePreview_) {
      cuePreview_ = false;
      if (!playLatched_) {
        jump(cue_);
        stopMotor(true);
      }
      playLatched_ = false;
    }
  }
  void buttonHot(int k, bool down) {
    if (!loaded_ || k < 0 || k >= kHotCues) return;
    auto& h = hot_[static_cast<size_t>(k)];
    if (down) {
      if (h < 0) {
        h = quantized(pos_);
        marksChanged();
        return;
      }
      const bool wasPlaying = motor_;
      jump(phaseJump(h));
      if (!wasPlaying) {
        hotPreview_ = k;
        playLatched_ = false;
        startMotor(true);
      }
    } else if (hotPreview_ == k) {
      hotPreview_ = -1;
      if (!playLatched_) {
        jump(h);
        stopMotor(true);
      }
      playLatched_ = false;
    }
  }
  void loopSet(double a, double b) {
    if (!loaded_ || !(b > a + 16)) {
      loopOn_ = false;
      return;
    }
    loopIn_ = a, loopOut_ = b, loopOn_ = true;
    if (savedIn_ != a || savedOut_ != b) {
      savedIn_ = a, savedOut_ = b;
      marksChanged();
    }
  }
  void autoLoop() {
    if (!loaded_) return;
    if (loopOn_) {
      loopOn_ = false;
      return;
    }
    const double size = loopBeats(ipar(PLoopSize));
    const double b0 = on(PQuantize) ? std::floor(beatAt(model(), pos_) + 1e-6) : beatAt(model(), pos_);
    loopSet(frameAt(model(), b0), frameAt(model(), b0 + size));
  }
  void loopResize(double factor) {
    if (!loopOn_) return;
    const double bi = beatAt(model(), loopIn_), bo = beatAt(model(), loopOut_);
    const double len = std::clamp((bo - bi) * factor, 1.0 / 32, 64.0);
    loopSet(loopIn_, frameAt(model(), bi + len));
  }
  void beatJump(double dir) {
    if (!loaded_) return;
    const double size = loopBeats(ipar(PLoopSize)) * dir;
    const double b = beatAt(model(), pos_);
    jump(frameAt(model(), b + size));
    if (loopOn_) loopSet(frameAt(model(), beatAt(model(), loopIn_) + size), frameAt(model(), beatAt(model(), loopOut_) + size));
  }

  void handDown() {
    if (!loaded_) return;
    if (!hand_) {
      hand_ = true;
      handPos_ = trX_ = jumpPending_ ? jumpTo_ : pos_;
      trV_ = vel_;
      handBase_ = handPos_;
    }
    softHand_ = false;
  }
  void handUp() {
    if (!hand_) return;
    hand_ = false;
    softHand_ = false;
    vel_ = trV_;
    rimMuteUntil_ = clock_ + static_cast<uint64_t>(sr_ * 0.5);
  }
  double framesPerTick() const { return srcRate() * kSecondsPerRev / std::max(8.0, aux_ ? aux_->m.map.ticksPerRev : 128.0); }
  void nudgeTicks(int ticks) {
    if (!loaded_) return;
    if (motor_ && !hand_) {
      const double tpr = std::max(8.0, aux_->m.map.ticksPerRev);
      nudge_ = std::clamp(nudge_ + ticks * 0.6 / tpr, -0.5, 0.5);
    } else if (!motor_) {
      // stopped: the wheel moves the record by hand (it lets go after a moment)
      if (!hand_) {
        handDown();
        softHand_ = true;
      }
      handPos_ += ticks * framesPerTick() * 0.5;
      softUntil_ = clock_ + static_cast<uint64_t>(sr_ * 0.08);
    }
  }

  // ---------------------------------------------------------------- inputs
  void input(const InEvent& e) {
    if (takeOn_ && e.control != CtlShift && e.control != CtlNone && controlInfo(e.control).type != TypeAbsolute) stopTake();  // the hand takes over
    const int c = e.control;
    switch (c) {
      case CtlPlay: buttonPlay(e.down); break;
      case CtlCue: buttonCue(e.down); break;
      case CtlSync:
        if (e.down) {
          toggleCtl(PSync);
          if (on(PSync) && motor_ && transport_) {
            motor_ = false;  // re-align the phase now
            startMotor(false);
          }
        }
        break;
      case CtlKeyLock: if (e.down) toggleCtl(PKeyLock); break;
      case CtlSlip: if (e.down) toggleCtl(PSlip); break;
      case CtlVinyl: if (e.down) toggleCtl(PVinyl); break;
      case CtlQuantize: if (e.down) toggleCtl(PQuantize); break;
      case CtlShift: shift_ = e.down; break;
      case CtlTouch:
        if (e.down && on(PVinyl)) handDown();
        else if (!e.down) handUp();
        break;
      case CtlJogPlatter:
        if (hand_ && !softHand_) handPos_ += e.ticks * framesPerTick();
        else if (clock_ >= rimMuteUntil_ || !on(PVinyl)) nudgeTicks(e.ticks);
        break;
      case CtlJogRim:
        if (!hand_ && clock_ >= rimMuteUntil_) nudgeTicks(e.ticks);
        break;
      case CtlJogSearch: searchTicks_ += e.ticks; break;
      case CtlPitch: setCtl(PPitch, static_cast<float>(e.value * 2.0 - 1.0)); break;
      case CtlVolume: setCtl(PVolume, e.value); break;
      case CtlXfader: setCtl(PXfader, e.value); break;
      case CtlEqHigh: setCtl(PEqHigh, static_cast<float>(eqDbOf(e.value))); break;
      case CtlEqMid: setCtl(PEqMid, static_cast<float>(eqDbOf(e.value))); break;
      case CtlEqLow: setCtl(PEqLow, static_cast<float>(eqDbOf(e.value))); break;
      case CtlFilter: setCtl(PFilter, static_cast<float>(std::fabs(e.value - 0.5) < 0.012 ? 0.0 : e.value * 2.0 - 1.0)); break;
      case CtlTrim: setCtl(PTrim, static_cast<float>(e.value * 24.0 - 12.0)); break;
      case CtlLoopIn:
        if (e.down && loaded_) pendingIn_ = quantized(pos_);
        break;
      case CtlLoopOut:
        if (e.down && loaded_) {
          if (loopOn_) loopOn_ = false;
          else if (pendingIn_ >= 0) loopSet(pendingIn_, quantized(pos_));
        }
        break;
      case CtlLoopAuto: if (e.down) autoLoop(); break;
      case CtlLoopHalve: if (e.down) loopResize(0.5); break;
      case CtlLoopDouble: if (e.down) loopResize(2.0); break;
      case CtlJumpBack: if (e.down) beatJump(-1); break;
      case CtlJumpFwd: if (e.down) beatJump(1); break;
      case CtlCensor:
        if (e.down && !censor_) {
          censor_ = true;
          vel_ = -std::fabs(vel_);
        } else if (!e.down && censor_) {
          censor_ = false;
          if (motor_) vel_ = std::fabs(vel_);
        }
        break;
      case CtlJumpStart:
        if (e.down) jump(0);
        break;
      case CtlBendDown: bend_ = e.down ? -0.04 : 0; break;
      case CtlBendUp: bend_ = e.down ? 0.04 : 0; break;
      default:
        if (c >= CtlHot1 && c < CtlHot1 + kHotCues) buttonHot(c - CtlHot1, e.down);
        else if (c >= CtlHotClear1 && c < CtlHotClear1 + kHotCues && e.down) {
          hot_[static_cast<size_t>(c - CtlHotClear1)] = -1;
          marksChanged();
        }
        break;
    }
  }

  void command(const Cmd& c) {
    switch (c.kind) {
      case CmdButton: {
        InEvent e;
        e.control = c.a;
        e.down = c.v != 0;
        input(e);
        break;
      }
      case CmdAbsolute: {
        InEvent e;
        e.control = c.a;
        e.type = TypeAbsolute;
        e.value = static_cast<float>(c.v);
        input(e);
        break;
      }
      case CmdHandDown:
        if (takeOn_) stopTake();
        handDown();
        break;
      case CmdHandMove:
        if (hand_) handPos_ = handBase_ + c.v * srcRate() * kSecondsPerRev;
        break;
      case CmdHandUp: handUp(); break;
      case CmdSeek:
        if (takeOn_) stopTake();
        jump(c.v);
        break;
      case CmdLoop:
        if (c.v < 0) loopOn_ = false;
        else loopSet(c.v, c.w);
        break;
      default: break;
    }
  }

  // Keys: 60..67 hot cues, 36..59 takes.
  void key(int k, bool down) {
    if (!loaded_) return;
    // a note (render() is split at every event) cuts the running step here, so
    // what it starts lands on its exact sample, whatever the block size
    if (!capturing_) cut();
    if (k >= kHotKeyFirst && k < kHotKeyFirst + kHotCues) {
      const int i = k - kHotKeyFirst;
      if (down) {
        if (takeOn_) stopTake();
        double h = hot_[static_cast<size_t>(i)];
        if (h < 0) {  // not set: bar i after the cue
          const int bpb = std::max(1, model().grid.beatsPerBar);
          h = frameAt(model(), beatAt(model(), cue_) + static_cast<double>(i * bpb));
        }
        jump(h);
        noteHot_ = i;
        motor_ = true;
        vel_ = baseSpeed();
      } else if (noteHot_ == i) {
        noteHot_ = -1;
        if (ipar(PNoteMode) == NoteGate) {
          motor_ = false;
          vel_ = 0;
        }
      }
      return;
    }
    if (k >= kTakeKeyFirst && k < kTakeKeyFirst + kTakeKeys) {
      if (down) {
        for (size_t t = 0; t < model().takes.size(); ++t)
          if (model().takes[t].key == k && !model().takes[t].pts.empty()) {
            startTake(static_cast<int>(t));
            break;
          }
      } else if (takeOn_ && takeKey_ == k) {
        stopTake();
      }
    }
  }

  // ---------------------------------------------------------------- takes
  void startTake(int index) {
    const Take& t = model().takes[static_cast<size_t>(index)];
    takeOn_ = true;
    takeIndex_ = index;
    takeKey_ = t.key;
    takeT_ = 0;
    takeStepOut_ = static_cast<double>(t.step) * (sr_ / t.sampleRate) * (t.tempo / tempo_);
    hand_ = softHand_ = false;
    cuePreview_ = false, hotPreview_ = -1, noteHot_ = -1, censor_ = false;
    loopOn_ = false;
    jump(t.startPos);
    vel_ = t.startRate;
    takeGain_ = t.startGain;
  }
  void stopTake() {
    if (!takeOn_) return;
    takeOn_ = false;
    motor_ = takeMotor_;
    vel_ = prevRate_;
    takeKey_ = -1;
  }

  void beginCapture() {
    capturing_ = true;
    capStartAt_ = -1;
    CaptureEvent e;
    e.kind = CaptureEvent::Start;
    e.tick = capStartTick_;
    e.tempo = tempo_;
    e.sampleRate = sr_;
    e.ppq = ppq_;
    e.startRate = vel_;
    e.pt.pos = jumpPending_ ? jumpTo_ : pos_;
    e.gain = lastGain_;
    pushCapture(e);
    capSteps_ = 0;
    capSamples_ = 0;
    if (keyOn_) keyRestart_ = true;  // the key lock engine starts fresh with the take (replays match)
  }
  void endCapture() {
    if (!capturing_) return;
    capturing_ = false;
    CaptureEvent e;
    e.kind = CaptureEvent::End;
    e.tick = capStartTick_ + static_cast<double>(capSamples_) * tempo_ * ppq_ / (60.0 * sr_);
    pushCapture(e);
  }
  void pushCapture(const CaptureEvent& e) {
    const size_t h = capHead_.load(std::memory_order_relaxed);
    if (h - capTail_.load(std::memory_order_acquire) >= capture_.size() - 1) {
      capLost_ = true;
      return;
    }
    if (capLost_ && e.kind == CaptureEvent::Point) return;
    CaptureEvent& slot = capture_[h % capture_.size()];
    slot = e;
    if (capLost_ && e.kind == CaptureEvent::End) slot.kind = CaptureEvent::Lost;  // the editor drops a take with holes
    capHead_.store(h + 1, std::memory_order_release);
    if (e.kind != CaptureEvent::Point) capLost_ = false;
  }

  // ---------------------------------------------------------------- the step
  struct Ghost {
    double pos = 0, rate = 0;
    int left = 0;
  };

  static double q256(double x) { return std::round(x * 256.0) / 256.0; }
  static float gq(double g) { return static_cast<float>(std::round(g * 4096.0) / 4096.0); }

  double faderGain() const {
    const double v = std::clamp(static_cast<double>(p_[PVolume]), 0.0, 1.0);
    double g = v;
    switch (ipar(PFaderCurve)) {
      case FcSmooth: g = v * v; break;
      case FcSharp: g = std::min(1.0, v * 6.0); break;
      default: break;
    }
    const int side = ipar(PXfSide);
    if (side != XfThru) {
      double x = std::clamp(static_cast<double>(p_[PXfader]), 0.0, 1.0);
      if (side == XfA) x = 1.0 - x;  // A is loud at 0
      const double c = std::clamp(static_cast<double>(p_[PXfCurve]), 0.0, 1.0);
      const double k = 1.0 + c * c * 40.0;
      g *= std::sin(std::min(1.0, x * k) * kPi * 0.5);
    }
    return g;
  }
  // Level that follows the platter's speed (a record makes no sound standing still).
  double speedGain(double rate) const {
    const double r = std::fabs(rate) / std::max(1e-9, unity());
    return std::sqrt(std::min(1.0, r / 0.5));
  }

  void computeStep() {
    const int len = kStep;
    stepLen_ = len;
    stepPos_ = 0;
    plan_.silent = !loaded_;
    if (!loaded_) return;
    const double fLen = static_cast<double>(len);
    const double x0 = pos_;
    const double v0 = vel_;
    double start = x0, x1 = x0, v1 = v0;
    bool jumped = false;
    uint8_t flags = 0;
    float gain = gq(faderGain());

    // soft hand (wheel while stopped) lets go
    if (softHand_ && clock_ >= softUntil_) handUp(), vel_ = trV_ = 0;
    // search: whole jumps
    if (searchTicks_ != 0) {
      jump(pos_ + searchTicks_ * srcRate() * 0.2);
      searchTicks_ = 0;
    }
    // slip: the ghost position keeps time while something holds the record
    const bool slipNow = on(PSlip) && (hand_ || censor_ || loopOn_ || hotPreview_ >= 0);
    if (slipNow && !slipping_) slipping_ = true, slipPos_ = x0;
    if (!slipNow && slipping_) {
      slipping_ = false;
      if (motor_) jump(slipPos_);
    }

    const bool replay = takeOn_;
    if (replay) {
      // ---- replay
      const Take& t = model().takes[static_cast<size_t>(takeIndex_)];
      const double adv = fLen / takeStepOut_;
      const double tEnd = takeT_ + adv;
      const bool exact = std::fabs(takeStepOut_ - kStep) < 1e-9 && std::fabs(takeT_ - std::round(takeT_)) < 1e-9 && len == kStep;
      if (jumpPending_) {  // the take's first step lands on its start
        start = jumpTo_;
        jumped = std::fabs(jumpTo_ - x0) > 0.5;
        jumpPending_ = false;
      }
      if (exact) {
        const auto i = static_cast<size_t>(std::llround(takeT_));
        if (i < t.pts.size()) {
          const TakePoint& p = t.pts[i];
          if (p.flags & TfJump) start = p.from, jumped = true;
          x1 = p.pos;
          gain = p.gain;
          flags = p.flags;
        }
      } else {
        auto at = [&](double tt, float& g, uint8_t& f) {
          if (tt <= 0) {
            g = t.startGain;
            f = 0;
            return t.startPos;
          }
          const auto i = static_cast<size_t>(std::min<double>(std::floor(tt), static_cast<double>(t.pts.size() - 1)));
          const double fr = std::min(1.0, tt - static_cast<double>(i));
          const TakePoint& p = t.pts[i];
          const double a = (p.flags & TfJump) ? p.from : (i ? t.pts[i - 1].pos : t.startPos);
          const float ga = i ? t.pts[i - 1].gain : t.startGain;
          g = ga + (p.gain - ga) * static_cast<float>(fr);
          f = p.flags;
          return a + (p.pos - a) * fr;
        };
        float g = 1;
        uint8_t f = 0;
        x1 = at(tEnd, g, f);
        gain = g;
        flags = static_cast<uint8_t>(f & ~TfJump);
        for (double k = std::floor(takeT_) + 1; k <= tEnd && k < static_cast<double>(t.pts.size()); k += 1)
          if (t.pts[static_cast<size_t>(k)].flags & TfJump) {
            start = t.pts[static_cast<size_t>(k)].from;
            jumped = true;
            flags |= TfJump;
          }
      }
      takeMotor_ = (flags & TfMotor) != 0;
      takeT_ = tEnd;
      v1 = (x1 - start) / fLen;
      motor_ = takeMotor_;
      if (takeT_ >= static_cast<double>(t.pts.size()) - 1e-9) {
        // the take is over: the deck carries on from where it left the record
        takeOn_ = false;
        takeKey_ = -1;
        vel_ = takeMotor_ ? v1 : 0.0;
      }
    } else {
      // ---- live physics
      if (jumpPending_) {
        start = jumpTo_;
        jumped = std::fabs(jumpTo_ - x0) > 1e-6;
        jumpPending_ = false;
      }
      if (hand_) {
        // alpha-beta tracker, critically damped, bandwidth from Hand feel (4 .. 16 Hz)
        const double f = 4.0 * std::pow(4.0, std::clamp(static_cast<double>(p_[PFeel]), 0.0, 1.0));
        const double wT = 2 * kPi * f * fLen / sr_;
        const double alpha = std::min(1.0, 2.0 * wT), beta = std::min(1.0, wT * wT);
        if (jumped) trX_ = start;
        const double pred = trX_ + trV_ * fLen;
        const double r = handPos_ - pred;
        trX_ = pred + alpha * r;
        trV_ += beta * r / fLen;
        x1 = trX_;
        v1 = trV_;
        flags |= TfHand;
        if (!jumped) start = x0;
      } else {
        const double base = baseSpeed();
        double target = motor_ ? base * (1.0 + bend_ + nudge_) : 0.0;
        if (censor_) target = -std::fabs(target);
        // phase lock to the project's beat (sync, motor running steadily, transport playing)
        // (not while a nudge or bend moves the phase on purpose)
        if (motor_ && on(PSync) && transport_ && !censor_ && bend_ == 0 && nudge_ == 0 && std::fabs(v0 - target) <= 0.05 * std::fabs(target) &&
            noteHot_ < 0 && !loopOn_) {
          const double sb = songBeat(static_cast<double>(clock_ - blockClock_));
          const double db = beatAt(model(), start);
          double err = (db - std::floor(db)) - (sb - std::floor(sb));
          if (err > 0.5) err -= 1;
          if (err < -0.5) err += 1;
          const double corr = std::clamp(-err * 0.6, -0.04, 0.04);
          target *= 1.0 + corr;
        }
        const double t = motor_ && std::fabs(target) >= std::fabs(v0) ? p_[PStartTime] : p_[PStopTime];
        const double slope = t > 1e-4 ? std::max(unity(), std::fabs(base)) * fLen / (static_cast<double>(t) * sr_) : 1e9;
        v1 = v0 + std::clamp(target - v0, -slope, slope);
        if (!motor_ && std::fabs(v1) < 1e-5 * unity()) v1 = 0;
        x1 = start + 0.5 * (v0 + v1) * fLen;
        if (motor_) flags |= TfMotor;
        // nudge decays (about 120 ms)
        nudge_ *= std::exp(-fLen / (0.12 * sr_));
        if (std::fabs(nudge_) < 1e-4) nudge_ = 0;
        // loop: wrap the step that crosses the loop's end (either direction)
        if (loopOn_ && loopOut_ > loopIn_) {
          const double L = loopOut_ - loopIn_;
          if (x1 >= loopOut_ && start < loopOut_ + 1) {
            start -= L, x1 -= L, jumped = true;
          } else if (x1 < loopIn_ && start >= loopIn_ - 1 && v1 < 0) {
            start += L, x1 += L, jumped = true;
          }
        }
      }
    }
    // the read head stays on the record (a little run-out each side)
    const double lim = frames() + srcRate() * 0.5;
    if (x1 > lim || x1 < -srcRate() * 0.5) {
      x1 = std::clamp(x1, -srcRate() * 0.5, lim);
      if (!hand_) v1 = 0, motor_ = motor_ && x1 < lim;
    }
    start = q256(start);
    x1 = q256(x1);
    if (jumped) flags |= TfJump;
    const double rate = (x1 - start) / fLen;

    // key lock: steady motor play between 0.5x and 1.9x
    const double rel = rate / std::max(1e-9, unity());
    // (a take replays its own choice, its last step included)
    bool wantKey = false;
    if (replay) wantKey = pool_ && (flags & TfKeyLock);
    else
      wantKey = pool_ && on(PKeyLock) && (flags & TfMotor) && !(flags & TfHand) && rel > 0.5 && rel < 1.9 && !censor_ &&
                std::fabs(v1 - v0) < 2e-3 * unity();
    if (wantKey) flags |= TfKeyLock;
    else flags &= static_cast<uint8_t>(~TfKeyLock);

    planStep(len, x0, start, x1, rate, v0, v1, jumped, wantKey, gain);

    // state for the next step
    if (!hand_) vel_ = v1;
    prevRate_ = rate;
    lastSpeedGain_ = plan_.sg1;
    pos_ = x1;
    if (slipping_ && motor_) slipPos_ += baseSpeed() * fLen;
    lastGain_ = gain;
    if (capturing_) {
      CaptureEvent e;
      e.kind = CaptureEvent::Point;
      e.pt.pos = x1;
      e.pt.from = jumped ? start : 0;
      e.pt.gain = gain;
      e.pt.flags = flags;
      pushCapture(e);
      ++capSteps_;
      capSamples_ += static_cast<uint64_t>(len);
    }
  }

  // Settings for the step just planned (ghosts, key lock engine, EQ / filter).
  void planStep(int len, double x0, double start, double x1, double rate, double v0, double v1, bool jumped, bool keyMode, float gain) {
    Plan& P = plan_;
    P.len = len, P.start = start, P.x1 = x1, P.rate = rate, P.v0 = v0, P.v1 = v1, P.key = keyMode;
    P.g0 = lastGain_, P.g1 = gain;
    P.sg0 = lastSpeedGain_, P.sg1 = speedGain(rate);
    P.hand = hand_;
    // a jump: the old motion carries on as a ghost and fades out
    if (jumped) {
      if (keyOn_ && pool_) {
        keyGhost_ = keyCur_;
      } else {
        ghost_.pos = x0;
        ghost_.rate = prevRate_;
        keyGhost_ = -1;
      }
      ghost_.left = kFade;
    }
    // the key lock engine: (re)start on entering it, on jumps and when a take starts
    if (keyMode) {
      if (!keyOn_ || jumped || keyRestart_) {
        if (keyOn_ && !jumped) {  // a fresh engine, the old one fades out
          keyGhost_ = keyCur_;
          ghost_.left = kFade;
        } else if (!keyOn_ && !jumped) {  // from the vinyl path: it fades out as a ghost
          ghost_.pos = x0;
          ghost_.rate = prevRate_;
          ghost_.left = kFade;
          keyGhost_ = -1;
        }
        if (keyGhost_ == keyCur_) keyCur_ = 1 - keyCur_;
        KeyPool& kp = *pool_;
        kp.rd[keyCur_].src = aux_->src.get();
        kp.rd[keyCur_].cursor = static_cast<int64_t>(std::floor(start));
        kp.v[keyCur_].reset();
        kp.v[keyCur_].start(readFrames, &kp.rd[keyCur_], rate, srcRate() / sr_, false);
        keyOn_ = true;
        keyRestart_ = false;
      }
    } else if (keyOn_) {
      // back to the vinyl path: the key lock engine fades out as the ghost
      keyGhost_ = keyCur_;
      ghost_.left = kFade;
      keyOn_ = false;
    }
    P.ghostRate = std::fabs(prevRate_) > 1e-9 ? std::fabs(prevRate_) : unity();
    // EQ / filter targets
    P.eqH = p_[PEqHigh] <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(p_[PEqHigh]);
    P.eqM = p_[PEqMid] <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(p_[PEqMid]);
    P.eqL = p_[PEqLow] <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(p_[PEqLow]);
    P.trim = dbToGain(p_[PTrim]);
    const double fz = p_[PFilter];
    P.wetT = std::clamp((std::fabs(fz) - 0.02) / 0.06, 0.0, 1.0);
    double fc = 20000;
    if (fz < 0) fc = 20000.0 * std::pow(60.0 / 20000.0, std::min(1.0, -fz));
    else if (fz > 0) fc = 20.0 * std::pow(9000.0 / 20.0, std::min(1.0, static_cast<double>(fz)));
    P.fg = std::tan(kPi * std::min(fc, sr_ * 0.45) / sr_);
    P.fk = 2.0 - 1.85 * std::clamp(static_cast<double>(p_[PFilterRes]), 0.0, 1.0);
    P.hp = fz > 0;
    P.smooth = 1.0 - std::exp(-1.0 / (0.004 * sr_));
  }

  // A note lands inside a step: the step ends here and the next one starts on this sample.
  void cut() {
    if (stepPos_ <= 0 || stepPos_ >= stepLen_ || plan_.silent) {
      if (stepPos_ <= 0) stepLen_ = 0;
      return;
    }
    const Plan& P = plan_;
    const double u = static_cast<double>(stepPos_) / P.len;
    pos_ = P.start + (P.x1 - P.start) * u;
    if (P.hand) trX_ = pos_;
    else if (!takeOn_) vel_ = P.v0 + (P.v1 - P.v0) * u;
    lastGain_ = static_cast<float>(P.g0 + (P.g1 - P.g0) * u);
    lastSpeedGain_ = P.sg0 + (P.sg1 - P.sg0) * u;
    stepLen_ = stepPos_;
  }

  // Renders samples [stepPos_, stepPos_ + count) of the planned step (adds to L / R).
  void runStep(float* L, float* R, int count) {
    if (plan_.silent) {
      stepPos_ += count;
      clock_ += static_cast<uint64_t>(count);
      return;
    }
    const Plan& P = plan_;
    const Sample& s = *aux_->src;
    const auto n = static_cast<int64_t>(s.frames());
    const float* xs[2] = {s.left.data(), s.right.data()};
    const pitch::SincTable& tab = pitch::table(pitch::Quality::Realtime);
    const double fLen = static_cast<double>(P.len);
    float kl[2][kStep], kg[2][kStep];
    if (P.key) {
      float* out[2] = {kl[0], kl[1]};
      pool_->v[keyCur_].render(out, count, P.rate, srcRate() / sr_, 0.0, readFrames, &pool_->rd[keyCur_]);
    }
    const bool keyGhostNow = keyGhost_ >= 0 && ghost_.left > 0 && pool_;
    if (keyGhostNow) {
      float* out[2] = {kg[0], kg[1]};
      pool_->v[keyGhost_].render(out, count, P.ghostRate, srcRate() / sr_, 0.0, readFrames, &pool_->rd[keyGhost_]);
    }
    const bool eqFlat = P.eqH == 1.0 && P.eqM == 1.0 && P.eqL == 1.0 && eqGain_[0] == 1.0 && eqGain_[1] == 1.0 && eqGain_[2] == 1.0;
    for (int c = 0; c < count; ++c) {
      const int j = stepPos_ + c;
      const double u = static_cast<double>(j) / fLen;
      double l = 0, r = 0;
      if (P.key) {
        l = kl[0][c], r = kl[1][c];
      } else {
        const double p = P.start + (P.x1 - P.start) * u;
        if (pitch::exactFrame(p, P.rate)) {  // whole frames at normal speed: the file itself
          const auto k = static_cast<int64_t>(std::floor(p + 0.5));
          const bool in = k >= 0 && k < n;
          l = in ? xs[0][k] : 0.0, r = in ? xs[1][k] : 0.0;
        } else {
          float o[2];
          pitch::readArrays<2>(xs, n, p, std::max(std::fabs(P.rate), 1e-6), tab, o);
          l = o[0], r = o[1];
        }
      }
      const double sg = P.sg0 + (P.sg1 - P.sg0) * u;
      l *= sg, r *= sg;
      if (ghost_.left > 0) {
        double gl = 0, gr = 0;
        if (keyGhostNow) {
          gl = kg[0][c], gr = kg[1][c];
        } else {
          float o[2];
          pitch::readArrays<2>(xs, n, ghost_.pos, std::max(std::fabs(ghost_.rate), 1e-6), tab, o);
          const double gsg = speedGain(ghost_.rate);
          gl = o[0] * gsg, gr = o[1] * gsg;
          ghost_.pos += ghost_.rate;
        }
        const double w = static_cast<double>(ghost_.left) / kFade;  // 1 -> 0
        const double wg = std::sin(w * kPi * 0.5), wm = std::cos(w * kPi * 0.5);
        l = l * wm + gl * wg;
        r = r * wm + gr * wg;
        if (--ghost_.left == 0) keyGhost_ = -1;
      }
      l *= P.trim, r *= P.trim;
      // isolator EQ (the bands sum back to the input exactly; flat skips it)
      for (int b = 0; b < 3; ++b) {
        const double tgt = b == 0 ? P.eqL : (b == 1 ? P.eqM : P.eqH);
        eqGain_[b] += (tgt - eqGain_[b]) * P.smooth;
        if (std::fabs(eqGain_[b] - tgt) < 1e-7) eqGain_[b] = tgt;
      }
      double bl, bm, bh;
      eq_.bands(0, l, bl, bm, bh);
      if (!eqFlat) l = bl * eqGain_[0] + bm * eqGain_[1] + bh * eqGain_[2];
      eq_.bands(1, r, bl, bm, bh);
      if (!eqFlat) r = bl * eqGain_[0] + bm * eqGain_[1] + bh * eqGain_[2];
      // filter (low-pass left of centre, high-pass right), faded in past the centre
      wet_ += (P.wetT - wet_) * P.smooth;
      if (wet_ < 1e-6 && P.wetT == 0.0) wet_ = 0;
      if (wet_ > 0) {
        double a, b, cc;
        filt_[0].run(l, P.fg, P.fk, a, b, cc);
        const double fl = P.hp ? cc : a;
        filt_[1].run(r, P.fg, P.fk, a, b, cc);
        const double fr = P.hp ? cc : a;
        l += (fl - l) * wet_;
        r += (fr - r) * wet_;
      } else {
        filt_[0].reset();
        filt_[1].reset();
      }
      const double g = P.g0 + (P.g1 - P.g0) * (static_cast<double>(j + 1) / fLen);
      const auto ol = static_cast<float>(l * g), orr = static_cast<float>(r * g);
      L[c] += ol;
      R[c] += orr;
      peak_ = std::max(peak_, std::max(std::fabs(ol), std::fabs(orr)));
    }
    stepPos_ += count;
    clock_ += static_cast<uint64_t>(count);
  }

  void publish() {
    const float peak = peak_;
    peak_ = 0;
    pubLoaded_.store(loaded_, std::memory_order_relaxed);
    pubPos_.store(pos_, std::memory_order_relaxed);
    pubSpeed_.store(prevRate_ / std::max(1e-9, unity()), std::memory_order_relaxed);
    pubMotor_.store(motor_, std::memory_order_relaxed);
    pubHand_.store(hand_ && !softHand_, std::memory_order_relaxed);
    pubSlipping_.store(slipping_, std::memory_order_relaxed);
    pubSlipPos_.store(slipping_ ? slipPos_ : -1, std::memory_order_relaxed);
    pubLoopOn_.store(loopOn_, std::memory_order_relaxed);
    pubLoopIn_.store(loopOn_ ? loopIn_ : savedIn_, std::memory_order_relaxed);
    pubLoopOut_.store(loopOn_ ? loopOut_ : savedOut_, std::memory_order_relaxed);
    pubSavedIn_.store(savedIn_, std::memory_order_relaxed);
    pubSavedOut_.store(savedOut_, std::memory_order_relaxed);
    pubCue_.store(cue_, std::memory_order_relaxed);
    pubCueLit_.store(cuePreview_ || (!motor_ && loaded_ && std::fabs(pos_ - cue_) < srcRate() * 0.01), std::memory_order_relaxed);
    for (int k = 0; k < kHotCues; ++k) pubHot_[static_cast<size_t>(k)].store(hot_[static_cast<size_t>(k)], std::memory_order_relaxed);
    uint32_t f = 0;
    if (on(PSync)) f |= 1;
    if (on(PKeyLock)) f |= 2;
    if (on(PSlip)) f |= 4;
    if (on(PVinyl)) f |= 8;
    if (on(PQuantize)) f |= 16;
    if (keyOn_) f |= 32;
    if (shift_) f |= 64;
    if (capturing_) f |= 128;
    pubFlags_.store(f, std::memory_order_relaxed);
    pubTempo_.store(effectiveBpm(), std::memory_order_relaxed);
    pubTake_.store(takeOn_ ? takeKey_ : -1, std::memory_order_relaxed);
    pubLevel_.store(peak, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------- state
  const PlatterAux* aux_ = nullptr;
  bool loaded_ = false;
  double sr_ = 44100, tempo_ = 120, blockTick_ = 0;
  int ppq_ = 96;
  uint64_t clock_ = 0, blockClock_ = 0;
  bool transport_ = false, recording_ = false, offline_ = false;
  std::array<float, PCount> p_{}, lastState_{}, ctlVal_{};
  std::array<uint64_t, PCount> ctlUntil_{};

  // deck
  double pos_ = 0, vel_ = 0, prevRate_ = 0;
  bool motor_ = false, hand_ = false, softHand_ = false, censor_ = false, shift_ = false;
  double handPos_ = 0, handBase_ = 0, trX_ = 0, trV_ = 0;
  uint64_t softUntil_ = 0, rimMuteUntil_ = 0;
  double nudge_ = 0, bend_ = 0;
  int searchTicks_ = 0;
  bool jumpPending_ = false;
  double jumpTo_ = 0;
  bool cuePreview_ = false, playLatched_ = false;
  int hotPreview_ = -1, noteHot_ = -1;
  double cue_ = 0;
  std::array<double, kHotCues> hot_{};
  double savedIn_ = -1, savedOut_ = -1, loopIn_ = -1, loopOut_ = -1, pendingIn_ = -1;
  bool loopOn_ = false, marksDirty_ = false;
  bool slipping_ = false;
  double slipPos_ = 0;
  struct Held {
    uint32_t id = 0;
    int key = -1;
  };
  std::array<Held, 32> held_{};

  // takes
  bool takeOn_ = false, takeMotor_ = false;
  int takeIndex_ = -1, takeKey_ = -1;
  double takeT_ = 0, takeStepOut_ = kStep;
  float takeGain_ = 1;
  bool capturing_ = false, capLost_ = false;
  int64_t capStartAt_ = -1;
  double capStartTick_ = 0;
  uint64_t capSteps_ = 0, capSamples_ = 0;
  static constexpr size_t kCapture = 32768;
  std::vector<CaptureEvent> capture_;
  std::atomic<size_t> capHead_{0}, capTail_{0};

  // render
  struct Plan {
    bool silent = true, key = false, hand = false, hp = false;
    int len = kStep;
    double start = 0, x1 = 0, rate = 0, v0 = 0, v1 = 0, sg0 = 0, sg1 = 0, ghostRate = 1;
    double g0 = 0, g1 = 0;
    double eqH = 1, eqM = 1, eqL = 1, trim = 1, wetT = 0, fg = 0.5, fk = 1.4, smooth = 0.01;
  };
  Plan plan_;
  float peak_ = 0;
  int stepPos_ = 0, stepLen_ = 0;
  Ghost ghost_;
  double lastSpeedGain_ = 0;
  float lastGain_ = 1;
  Isolator eq_;
  double eqRate_ = 0;
  double eqGain_[3] = {1, 1, 1};
  Svf filt_[2];
  double wet_ = 0;
  KeyPool* pool_ = nullptr;
  std::atomic<KeyPool*> pending_{nullptr}, retired_{nullptr};
  double uiRate_ = 0;
  bool keyOn_ = false, keyRestart_ = false;
  int keyCur_ = 0, keyGhost_ = -1;

  // inputs
  Decoder decoderHub_, decoderRaw_;
  uint64_t hubPos_ = 0;
  std::array<Cmd, 256> cmds_{};
  std::atomic<size_t> cmdHead_{0}, cmdTail_{0};

  // published
  std::atomic<bool> pubLoaded_{false}, pubMotor_{false}, pubHand_{false}, pubSlipping_{false}, pubLoopOn_{false}, pubCueLit_{false};
  std::atomic<double> pubPos_{0}, pubSpeed_{0}, pubSlipPos_{-1}, pubLoopIn_{-1}, pubLoopOut_{-1}, pubSavedIn_{-1}, pubSavedOut_{-1}, pubCue_{0},
      pubTempo_{0};
  std::array<std::atomic<double>, kHotCues> pubHot_{};
  std::atomic<uint32_t> pubFlags_{0};
  std::atomic<int> pubTake_{-1};
  std::atomic<float> pubLevel_{0};
  std::atomic<uint32_t> raw_{0}, rawSeq_{0};
  std::array<std::atomic<uint32_t>, PCount> mirSeq_{};
  std::array<std::atomic<float>, PCount> mirVal_{};
  std::atomic<uint32_t> marksSeq_{0};
};

PlatterGen* deckOf(Generator* g) { return dynamic_cast<PlatterGen*>(g); }
const PlatterGen* deckOf(const Generator* g) { return dynamic_cast<const PlatterGen*>(g); }

}  // namespace

// ================================================================== pack entry points
std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == kType) return std::make_unique<PlatterGen>();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  aux.samples.clear();
  aux.custom.reset();
  const std::string path = pathOf(state.data);
  if (path.empty()) return true;
  SamplePtr src = sampleCache().get(path);
  aux.samples.assign(1, src);
  if (!src || src->frames() == 0) return true;
  (void)pitch::table(pitch::Quality::Realtime);
  aux.custom = cachedAux(state.data, src);
  return true;
}

bool liveView(const Generator* g, LiveView& out) {
  const PlatterGen* d = deckOf(g);
  if (!d) return false;
  d->view(out);
  return true;
}

bool mirror(const Generator* g, Mirror& out) {
  const PlatterGen* d = deckOf(g);
  if (!d) return false;
  d->mirrorOut(out);
  return true;
}

bool marks(const Generator* g, Marks& out) {
  const PlatterGen* d = deckOf(g);
  if (!d) return false;
  d->marksOut(out);
  return true;
}

bool command(Generator* g, const Cmd& c) {
  PlatterGen* d = deckOf(g);
  return d && d->pushCmd(c);
}

int drainCapture(Generator* g, CaptureEvent* out, int max) {
  PlatterGen* d = deckOf(g);
  return d ? d->drain(out, max) : 0;
}

}  // namespace gl::packs::platter

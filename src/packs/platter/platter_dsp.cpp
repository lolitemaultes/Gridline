// Gridline — "platter" pack: the runtime (gl.platter): two decks and the mixer.
//
// Everything advances in control steps of kStep output samples (shorter when an
// event cuts a step), whatever the host block size, so the setup renders the
// same audio for any block size and a recorded take replays sample for sample.
// Both decks share the step grid: a note that cuts a step cuts it on both.
// Per step each deck decides where its read head ends (hand tracking, motor,
// loops, jumps) and its level; inside the step the head moves on a cubic
// (Hermite) path whose speed starts at the speed the last step ended with and
// ends at this step's mean speed, so the speed never jumps (the curve depends
// on the stored positions only: takes replay it exactly). Every sample is read
// through Gridline's band-limited kernel at its own speed (any direction, down
// to zero), so a direction change is a continuous stop and turn, never a
// click. Jumps crossfade from a ghost reader that keeps the old motion for a
// few milliseconds.
//
// Physics (per deck). Scratching follows the approach of Mixxx (GPL-2.0-or-
// later, see NOTICE.md), re-done here on Platter's sample-exact step grid:
//   jog     a touched wheel moves the record after the hand: every step the
//           ticks that arrived (controller MIDI is stamped when it arrives and
//           laid out over the next block: the touch, the ticks and the release
//           each on their own sample) feed a second order tracker whose speed
//           is the platter's. Its poles are the wheel's alpha-beta pair (per
//           controller profile, set at 1 ms and mapped to the step length so
//           any rate and block size give the same curve); Hand feel speeds them
//           up or slows them down, and so does the hand itself (J-197): a quick
//           flick, reported every millisecond or two, is followed within a few
//           milliseconds, a slow drag on a coarse wheel keeps the full smoothing.
//           The record always ends where the hand put it. A touch catches the
//           record at its own speed (a playing record stops within ~50 ms, never
//           swinging back), a still hand lets it settle to rest (silence), a
//           release moves the hand on as the motor would until the record has
//           the motor's speed (or rest), then the motor has it;
//   mouse   a drag on the record or the waveform sets a target travel; every
//           16 ms a loop compares it with the record's own travel and sets a
//           new speed (a smoothed error times a gain), held through a late
//           mouse update for up to 40 ms and reached over 5.8 ms, so a 60 Hz
//           mouse never makes steps. A release faster than 2.5x throws the
//           record: it slows exponentially (100x to the motor speed in 1 s);
//   level   a record's level follows its speed (-6 dB at 0.3x, silent at rest
//           and through a turn), per sample;
//   motor   playing pulls the platter to its speed at a torque set by Start
//           time, stopping brakes it at Stop time; a throw keeps its speed and
//           the motor catches it;
//   nudge   rim turns (and the platter top in CDJ mode) bend the speed for a
//           moment; while stopped they move the record by hand.
//   sync    the tempo and beat phase follow the project, or the other deck.
//
// Mixer: per channel trim, isolator EQ with kills, filter, channel fader (with
// its curve), crossfader (curve, cut, reverse, per deck assign), master level.
//
// Audio thread: no allocation, no locks, no I/O. Inputs: the controller hub
// (device MIDI, app), raw channel MIDI (plugin builds), notes, and editor
// commands through a single-producer ring.
#include "../Packs.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include "../../core/Automation.h"
#include "../../engine/TimeStretch.h"
#include "../../engine/pitch/Kernel.h"
#include "platter.h"

namespace gl::packs::platter {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFade = 128;  // jump / path crossfade (samples)
const double kInvLog5 = 1.0 / std::log(5.0);
constexpr double kGlideSec = 256.0 / 44100.0;  // a scratch speed change is reached over this (one 256-frame callback at 44.1 kHz)

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
// J-159: the parameters' descriptors (first used on the UI thread: the generator's constructor).
const ParamDesc& descOf(int i) {
  static const std::vector<ParamDesc> d = [] {
    std::vector<ParamDesc> x(PCount);
    if (const PluginInfo* p = findPlugin(kType))
      for (int k = 0; k < PCount && k < static_cast<int>(p->params.size()); ++k) x[static_cast<size_t>(k)] = p->params[static_cast<size_t>(k)];
    return x;
  }();
  return d[static_cast<size_t>(i)];
}
// A value as a lane stores it (normalized float), and the value a lane point gives back
// (what the engine writes for automation): a recorded performance uses these live, so
// its lanes give the decks exactly the values they had.
float normOf(int i, double v) { return static_cast<float>(descOf(i).toNormalized(v)); }
float valueOfN(int i, double n) { return static_cast<float>(descOf(i).fromNormalized(n)); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double masterGainOf(double db) { return db <= kMasterOffDb + 1e-3 ? 0.0 : dbToGain(db); }

// ================================================================== aux
struct PlatterAux final : AuxData {
  std::vector<SamplePtr> srcs;  // Model::files order (null = missing)
  Model m;
  uint32_t device = 0;  // hub id of the bound device (0 = any)
  JogFilter jog;        // the controller's jog scratch filter
  const Sample* file(int f) const {
    const Sample* s = f >= 0 && f < static_cast<int>(srcs.size()) ? srcs[static_cast<size_t>(f)].get() : nullptr;
    return s && s->frames() > 0 ? s : nullptr;
  }
  const Sample* deckSrc(int d) const { return file(m.deck[static_cast<size_t>(d)].file); }
};

std::shared_ptr<const PlatterAux> cachedAux(const std::string& data, const Model& parsed, const std::vector<SamplePtr>& srcs) {
  static std::mutex mu;
  static std::map<std::string, std::weak_ptr<const PlatterAux>> cache;
  std::lock_guard<std::mutex> lk(mu);
  for (auto it = cache.begin(); it != cache.end();) it = it->second.expired() ? cache.erase(it) : std::next(it);
  std::string key = data;
  for (const auto& s : srcs) key += "|" + std::to_string(reinterpret_cast<uintptr_t>(s.get()));
  if (auto it = cache.find(key); it != cache.end())
    if (auto p = it->second.lock()) return p;
  auto x = std::make_shared<PlatterAux>();
  x->srcs = srcs;
  x->m = parsed;
  for (int d = 0; d < kDecks; ++d) {
    DeckModel& dm = x->m.deck[static_cast<size_t>(d)];
    const Sample* s = x->file(dm.file);
    if (s && dm.srcFrames != static_cast<int64_t>(s->frames())) {
      // a new or changed file: a flat grid until the editor stores the analysis
      const int f = dm.file;
      dm = deckModel(*s, seltmann::flatGrid(*s, 120), false);
      dm.file = f;
    }
  }
  x->device = hub::deviceId(x->m.map.device);
  x->jog = jogFilterOf(x->m.map.profile);
  cache[key] = x;
  return x;
}

// ================================================================== DSP pieces
// TPT state-variable filter (one channel).
struct Svf {
  double ic1 = 0, ic2 = 0;
  void reset() { ic1 = ic2 = 0; }
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
void readFrames(void* ctx, float* const* dst, int frames) {
  auto* r = static_cast<Reader*>(ctx);
  const int64_t n = r->src ? static_cast<int64_t>(r->src->frames()) : 0;
  for (int i = 0; i < frames; ++i) {
    const int64_t k = r->cursor + i;
    const bool in = k >= 0 && k < n;
    dst[0][i] = in ? r->src->left[static_cast<size_t>(k)] : 0.f;
    dst[1][i] = in ? r->src->right[static_cast<size_t>(k)] : 0.f;
  }
  r->cursor += frames;
}
// Two voices per deck (the current one and the one fading out).
struct KeyPool {
  double rate = 0;
  stretch::Voice v[kDecks][2];
  Reader rd[kDecks][2];
};

// ================================================================== shared state
// What both decks read from the setup (written by the generator on the audio thread).
struct Shared {
  double sr = 44100, tempo = 120, blockTick = 0;
  int ppq = 96;
  uint64_t clock = 0, blockClock = 0;
  double evAt = 0;     // sample time of the input being handled (hand events use it)
  double evDelay = 0;  // jog ticks take effect this much later (live input: one block, so a block's ticks keep their spacing)
  bool transport = false;
  std::array<float, PCount> p{}, ctlVal{};
  std::array<uint64_t, PCount> ctlUntil{};
  std::array<std::atomic<uint32_t>, PCount> mirSeq{};
  std::array<std::atomic<float>, PCount> mirVal{};
  const PlatterAux* aux = nullptr;
  KeyPool* pool = nullptr;
  // J-159: performances. `strict` while a pass is captured as a performance or lanes are
  // followed: inputs act on step starts and the decks use canonical values (what a lane
  // gives back): `p` is set from `raw` (or the lanes) at each step start only.
  std::array<float, PCount> raw{};
  bool strict = false;
  bool replay = false;  // following: the performance lanes (and command keys) drive the decks
  // Transport tick at a sample (an anchor set where the position jumped, so the same
  // sample gives the same tick whatever the block sizes).
  double anchorTick = 0;
  uint64_t anchorClock = 0;
  double tickAt(double clk) const { return anchorTick + (clk - static_cast<double>(anchorClock)) * tempo * ppq / (60.0 * sr); }
  // The decks' lane values this step (follow): has, normalized value
  std::array<bool, PCount> laneHas{};
  std::array<double, PCount> laneN{};

  double songBeat(double sampleInBlock) const { return (blockTick + sampleInBlock * tempo * ppq / (60.0 * sr)) / static_cast<double>(ppq); }
  // A controller moved parameter p: it wins over the stored value for a moment
  // (the editor writes it into the parameter and the automation).
  void setCtl(int i, float v) {
    const auto k = static_cast<size_t>(i);
    ctlVal[k] = v;
    ctlUntil[k] = clock + static_cast<uint64_t>(sr * 0.25) + 1;
    raw[k] = v;
    if (!strict) p[k] = v;
    mirVal[k].store(v, std::memory_order_relaxed);
    mirSeq[k].fetch_add(1, std::memory_order_release);
  }
};

// ================================================================== one deck
struct Ghost {
  double pos = 0, rate = 0;
  int left = 0;
};

// J-163: what a recorded pass's state lanes say for one step of a deck (-1: no lane plays).
struct DriveStep {
  bool chase = false;  // pick the pass up here: the deck takes the lanes' state at once
  int play = -1, touch = -1, hand = -1, loop = -1, rev = -1, bend = -1;
  bool instant = false;  // the play lane: the motor started / stopped at once on this step
  bool hasIn = false, hasOut = false;
  double loopIn = 0, loopOut = 0;
  bool jump = false;  // the position lane jumps on this step (to jumpTo)
  double jumpTo = 0;
  bool check = false;  // the position lane plays: lanePos is where it says the record is
  double lanePos = 0;
  bool hasPos = false;  // chase: the record's place and speed on this step (exact on a steady run)
  double pos = 0, vel = 0;
  // chase in a jog scratch: the hand's last steps (the wheel's travel per step, in seconds of
  // record, from the speed warmV at the first), so the jog filter goes on as it went
  static constexpr int kWarm = 2048;  // (1.5 s of steps)
  int warmN = 0;
  double warmV = 0;
  const double* warmDx = nullptr;  // (the setup's own buffer)
};

class Deck {
 public:
  void init(int index, Shared* s, const Deck* other) {
    idx_ = index;
    sh_ = s;
    other_ = other;
    hot_.fill(-1);
    for (auto& h : pubHot_) h.store(-1);
  }

  // ---------------------------------------------------------------- parameters
  float P(int d) const { return sh_->p[static_cast<size_t>(deckParam(idx_, d))]; }
  bool on(int d) const { return P(d) >= 0.5f; }
  int ipar(int d) const { return static_cast<int>(std::lround(P(d))); }
  void setCtl(int d, float v) { sh_->setCtl(deckParam(idx_, d), v); }
  void toggleCtl(int d) { setCtl(d, on(d) ? 0.f : 1.f); }

  // ---------------------------------------------------------------- source
  // The aux changed: the deck's track (and model) may have changed.
  void adopt(const PlatterAux* aux) {
    // (one-deck layout: deck B is off)
    const Sample* ns = aux && (idx_ == 0 || aux->m.twoDecks()) ? aux->deckSrc(idx_) : nullptr;
    const bool fileChanged = ns != src_;
    src_ = ns;
    dm_ = aux ? &aux->m.deck[static_cast<size_t>(idx_)] : nullptr;
    fileIdx_ = dm_ ? dm_->file : -1;
    if (dm_) adoptMarks(fileChanged);
    if (fileChanged) {
      motor_ = false, vel_ = 0, ghost_.left = 0, loopOn_ = false, fresh_ = true;
      scratchOff();
      cuePreview_ = false, hotPreview_ = -1, noteHot_ = -1, censor_ = false, slipping_ = false;
      jumpPending_ = false;
      pos_ = src_ ? cue_ : 0;
      keyOn_ = false;
    }
  }
  bool loaded() const { return src_ != nullptr; }
  // The sample the head reads now (a take plays its own file).
  const Sample* rs() const { return inTake_ ? takeSrc_ : src_; }
  double srcRate() const { return rs() ? rs()->sampleRate : sh_->sr; }
  double frames() const { return rs() ? static_cast<double>(rs()->frames()) : 0.0; }
  double unity() const { return srcRate() / sh_->sr; }  // read speed of the original tempo (frames per output sample)
  const DeckModel& model() const { return *dm_; }
  bool gridOk() const { return dm_ && src_ && !inTake_; }

  // ---------------------------------------------------------------- tempo
  double ownBpm() const { return dm_ && src_ ? bpmOf(model()) * (1.0 + P(DPitch) * pitchRangeOf(ipar(DPitchRange))) : 0.0; }
  // Speed of the motor (frames per output sample) at the current position.
  double baseSpeed() const {
    if (!loaded()) return 0;
    if (on(DSync) && leader_ >= 0 && syncBpm_ > 1) {
      const double b = beatAt(model(), pos_);
      const double fb = std::floor(b);
      double len = frameAt(model(), fb + 1) - frameAt(model(), fb);
      if (!(len > 1)) len = 60.0 * srcRate() / bpmOf(model());
      return len / (sh_->sr * 60.0 / syncBpm_);
    }
    return unity() * (1.0 + P(DPitch) * pitchRangeOf(ipar(DPitchRange)));
  }
  double effectiveBpm() const {
    if (!loaded()) return 0;
    if (on(DSync) && leader_ >= 0 && syncBpm_ > 1) return syncBpm_;
    return ownBpm();
  }
  double quantized(double f) const { return on(DQuantize) && loaded() ? nearestBeatFrame(model(), f) : f; }
  // A jump target that keeps the beat phase while playing (quantize).
  double phaseJump(double target) const {
    if (!on(DQuantize) || !motor_ || !loaded()) return target;
    const double bNow = beatAt(model(), pos_), bT = beatAt(model(), target);
    const double ph = bNow - std::floor(bNow);
    return frameAt(model(), std::round(bT) + ph);
  }
  // The beat phase (0..1) this deck locks to, false when there is none right now.
  bool leaderPhase(double sampleInBlock, double& phase) const {
    if (leader_ == kLeadProject) {
      if (!sh_->transport) return false;
      // (strict: the tick of this very sample, the same whatever the block sizes)
      const double sb = sh_->strict ? sh_->tickAt(static_cast<double>(sh_->clock)) / sh_->ppq : sh_->songBeat(sampleInBlock);
      phase = sb - std::floor(sb);
      return true;
    }
    if (leader_ == kLeadDeck && other_ && other_->motor_ && other_->loaded() && !other_->inTake_) {
      phase = other_->startBeat_ - std::floor(other_->startBeat_);
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------- marks
  void adoptMarks(bool fileChanged) {
    const DeckModel& m = *dm_;
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

  // ---------------------------------------------------------------- transport actions
  void jump(double target) {
    if (!loaded()) return;
    target = std::clamp(target, -srcRate() * 2, frames() + srcRate() * 2);
    jumpTo_ = target;
    jumpPending_ = true;  // (the mouse loop counts the record's travel, never its jumps)
  }
  void startMotor(bool instant) {
    if (!motor_ && on(DSync) && on(DQuantize) && loaded() && !hand_) {
      // land in phase with what the deck follows
      double want = 0;
      if (leaderPhase(static_cast<double>(sh_->clock - sh_->blockClock), want)) {
        if (leader_ == kLeadDeck) want = beatAt(other_->model(), other_->pos_) - std::floor(beatAt(other_->model(), other_->pos_));
        const double db = beatAt(model(), jumpPending_ ? jumpTo_ : pos_);
        double tgt = std::floor(db) + want;
        if (tgt - db > 0.5) tgt -= 1;
        if (db - tgt > 0.5) tgt += 1;
        jump(frameAt(model(), tgt));
      }
    }
    motor_ = true;
    if (instant) vel_ = baseSpeed(), motorSnap_ = true;
  }
  void stopMotor(bool instant) {
    motor_ = false;
    if (instant) vel_ = 0, motorSnap_ = true;
  }

  void buttonPlay(bool down) {
    if (!down) return;
    if (cuePreview_ || hotPreview_ >= 0) {
      playLatched_ = true;
      return;
    }
    if (motor_) stopMotor(P(DStopTime) <= 0.001f);
    else startMotor(P(DStartTime) <= 0.001f);
  }
  void buttonCue(bool down) {
    if (!loaded()) return;
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
    if (!loaded() || k < 0 || k >= kHotCues) return;
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
    if (!loaded() || !(b > a + 16)) {
      loopOn_ = false;
      return;
    }
    // (J-163: on the read head's 1/256 frame grid, so a loop lane holds them exactly)
    a = q256(a), b = q256(b);
    loopIn_ = a, loopOut_ = b, loopOn_ = true;
    if (savedIn_ != a || savedOut_ != b) {
      savedIn_ = a, savedOut_ = b;
      marksChanged();
    }
  }
  void autoLoop() {
    if (!loaded()) return;
    if (loopOn_) {
      loopOn_ = false;
      return;
    }
    const double size = loopBeats(ipar(DLoopSize));
    const double b0 = on(DQuantize) ? std::floor(beatAt(model(), pos_) + 1e-6) : beatAt(model(), pos_);
    loopSet(frameAt(model(), b0), frameAt(model(), b0 + size));
  }
  void loopResize(double factor) {
    if (!loopOn_) return;
    const double bi = beatAt(model(), loopIn_), bo = beatAt(model(), loopOut_);
    const double len = std::clamp((bo - bi) * factor, 1.0 / 32, 64.0);
    loopSet(loopIn_, frameAt(model(), bi + len));
  }
  void beatJump(double dir) {
    if (!loaded()) return;
    const double size = loopBeats(ipar(DLoopSize)) * dir;
    const double b = beatAt(model(), pos_);
    jump(frameAt(model(), b + size));
    if (loopOn_) loopSet(frameAt(model(), beatAt(model(), loopIn_) + size), frameAt(model(), beatAt(model(), loopOut_) + size));
  }

  // ---------------------------------------------------------------- scratching
  // (After the approach of Mixxx: see the notes at the top of the file.)
  // Hand feel: 0.5 is the standard speed of the scratch filters; loose halves
  // it (smoother, later), tight doubles it (quicker, closer to the hand).
  double feelK() const { return std::pow(2.0, 2.0 * (std::clamp(static_cast<double>(P(DFeel)), 0.0, 1.0) - 0.5)); }
  // Jog sensitivity: record travel per jog tick (1 = the record turns as far as the wheel).
  double jogSens() const { return std::clamp(static_cast<double>(P(DJog)), 0.25, 2.0); }
  double ticksPerRev() const { return std::max(8.0, sh_->aux ? sh_->aux->m.map.ticksPerRev : 128.0); }
  // Seconds of record (at its own speed) per jog tick: a 33 1/3 rpm turn is 1.8 s.
  double secondsPerTick() const { return kSecondsPerRev / ticksPerRev(); }
  bool scratching() const { return scr_ != ScrOff; }
  // The speed the record goes back to when the hand lets go (x its own speed).
  double releaseSpeed() const {
    if (!motor_) return 0.0;
    return baseSpeed() * (1.0 + bend_ + nudge_) / std::max(1e-9, unity());
  }
  double speedNow() const { return vel_ / std::max(1e-9, unity()); }

  // -- jog: ticks -> the record follows the hand (J-197)
  // The record's place follows the hand's (the sum of the ticks) through a second order
  // tracker: `jogE_` is how far the record is from the hand (seconds of record), `jogV_` its
  // speed. Its two poles are the wheel's alpha-beta pair (given per millisecond, as Mixxx's
  // mappings give it: a slow drag feels as it did), carried to the step length and scaled by
  // Hand feel and by the speed-up below, so the response in time is the same at any rate.
  // Unlike the alpha-beta form, the record always ends where the hand is (whatever the gains
  // did on the way), so the gains can change from step to step.
  // Speed-up: a coarse wheel turned slowly needs the full smoothing (a tick every 15-40 ms
  // would otherwise step), but a fast flick sends a message every millisecond or two, which
  // carries enough to follow it closely. From the last few steps that moved, the smoothing
  // shrinks to about two tick periods (and three message gaps), down to an eighth, within a
  // millisecond or so; it comes back over about 30 ms once the hand slows. Only the travel
  // per step goes in, so a live pass, its capture and its replay from the wheel lane all run
  // the same.
  static constexpr double kJogTau0 = 0.016;        // the standard filter's time scale (s)
  static constexpr double kJogTickSpan = 2.0;      // smoothing >= this many tick periods
  static constexpr double kJogGapSpan = 3.0;       // ... and this many gaps between moving steps
  static constexpr double kJogMaxSpeedUp = 8.0;
  static constexpr double kJogSpeedUpRise = 0.001;  // the speed-up comes over this (s)
  static constexpr double kJogSpeedUpFall = 0.03;   // back to the standard smoothing (s)
  static constexpr int kJogHist = 3;
  // The wheel's poles per millisecond (z^2 - (2 - a - b) z + (1 - a)), as logs (cached).
  void jogPoles() {
    const double a = sh_->aux ? sh_->aux->jog.alpha : JogFilter{}.alpha, b = sh_->aux ? sh_->aux->jog.beta : JogFilter{}.beta;
    if (a == jpA_ && b == jpB_) return;
    jpA_ = a, jpB_ = b;
    using C = std::complex<double>;
    const double s = 2.0 - a - b, q = 1.0 - a;
    const C root = std::sqrt(C(s * s - 4.0 * q, 0.0));
    const C z[2] = {(C(s, 0.0) + root) * 0.5, (C(s, 0.0) - root) * 0.5};
    for (int i = 0; i < 2; ++i) {
      jpZero_[i] = std::abs(z[i]) < 1e-12;
      jpLog_[i] = jpZero_[i] ? C(0.0, 0.0) : std::log(z[i]);
    }
  }
  // The tracker's gains for a step of `len` samples: e' = e - dx + (u + u') / 2 with
  // u' = (1 - B) u - A (e - dx) (u: the speed times the step), whose poles are w0, w1:
  // A = (1 - w0)(1 - w1), B = 1.5 - (w0 + w1) / 2 - w0 w1 / 2. `slow`: the slower pole.
  void jogGains(int len, double& A, double& B, double& slow) {
    jogPoles();
    using C = std::complex<double>;
    const double ratio = feelK() * jogS_ * static_cast<double>(len) / sh_->sr / 0.001;
    C w[2];
    for (int i = 0; i < 2; ++i) w[i] = jpZero_[i] ? C(0.0, 0.0) : std::exp(jpLog_[i] * ratio);
    const double T = (w[0] + w[1]).real(), D = (w[0] * w[1]).real();
    A = 1.0 - T + D;
    B = 1.5 - 0.5 * T - 0.5 * D;
    slow = std::max(std::abs(w[0]), std::abs(w[1]));
  }
  void jogReset() {
    jogS_ = 1.0;
    jogSince_ = kJogTau0;
    jogHistN_ = 0;
  }
  // The speed-up from a step's travel `dx` (seconds of record) after `len` samples.
  void jogRate(double dx, bool moved, int len) {
    const double dt = static_cast<double>(len) / sh_->sr;
    double target = 1.0;
    if (moved) {
      for (size_t i = kJogHist - 1; i > 0; --i) jogGap_[i] = jogGap_[i - 1], jogTk_[i] = jogTk_[i - 1];
      jogGap_[0] = std::min(jogSince_ + dt, kJogTau0);
      jogTk_[0] = std::fabs(dx) / std::max(1e-12, secondsPerTick() * jogSens());
      jogHistN_ = std::min(jogHistN_ + 1, kJogHist);
      jogSince_ = 0;
      double g = 0, n = 0;
      for (int i = 0; i < jogHistN_; ++i) g += jogGap_[static_cast<size_t>(i)], n += jogTk_[static_cast<size_t>(i)];
      g /= jogHistN_, n /= jogHistN_;
      const double tau = std::max(kJogTickSpan * g / std::max(n, 1e-9), kJogGapSpan * g);
      target = std::clamp(kJogTau0 / std::max(tau, 1e-9), 1.0, kJogMaxSpeedUp);
    } else {
      jogSince_ = std::min(jogSince_ + dt, kJogTau0);
    }
    if (target > jogS_) jogS_ += (target - jogS_) * (1.0 - std::exp(-dt / kJogSpeedUpRise));  // (not on one quick message)
    else jogS_ = std::max(target, 1.0 + (jogS_ - 1.0) * std::exp(-dt / kJogSpeedUpFall));
  }
  // The wheel is touched: scratching starts at the record's present speed. The hand
  // catches it: the record runs on to a stop on the tracker's slow mode (never back).
  void jogTouch(bool soft) {
    if (!loaded()) return;
    const bool already = scr_ == ScrJog && hand_;  // (a soft hand that becomes a touch keeps going)
    hand_ = true;
    softHand_ = soft;
    if (already) {
      jogRelease_ = false;
      return;
    }
    if (scr_ != ScrJog) tqTail_ = tqHead_;  // (no ticks of an earlier hand are left)
    jogRelease_ = false;
    jogV_ = speedNow();
    jogCatch();
    scr_ = ScrJog;
  }
  // The record at speed jogV_ under a still hand: the place it stops at is the hand's (the
  // state on the tracker's slow mode, so it slows down without swinging back).
  void jogCatch() {
    double A = 0, B = 0, w = 0;
    jogGains(kStep, A, B, w);
    const double den = 1.0 - 0.5 * A - w;
    jogE_ = std::fabs(den) > 1e-9 ? -(1.0 - 0.5 * B) * jogV_ * (static_cast<double>(kStep) / sh_->sr) / den : 0.0;
  }
  // The wheel is let go: the filter glides to the motor speed (or to rest).
  void jogLetGo(bool muteRim) {
    if (!hand_) return;
    hand_ = false;
    softHand_ = false;
    if (scr_ == ScrJog) {
      jogRelease_ = true;
      releaseTo_ = releaseSpeed();
    }
    // a wheel still spinning after the release does not bend the record for a moment
    if (muteRim) rimMuteUntil_ = sh_->clock + static_cast<uint64_t>(sh_->sr * 0.9);
  }
  // Jog ticks (already scaled) for the filter, at their sample time.
  void jogTicks(double ticks) {
    if (wheelLane_) return;  // (J-159: a wheel travel lane gives the filter its travel)
    const size_t h = tqHead_;
    const double sec = ticks * secondsPerTick();
    if (h - tqTail_ >= tq_.size()) {  // (full: the newest takes them, nothing is lost)
      tq_[(h - 1) % tq_.size()].sec += sec;
      return;
    }
    tq_[h % tq_.size()] = Tick{sh_->evAt + sh_->evDelay, sec};
    tqHead_ = h + 1;
  }
  // One step of the jog tracker; the speed at the step's end (x the record's own speed).
  double jogStep(int len, double t0) {
    double dx = 0;
    bool moved = false;
    while (tqTail_ != tqHead_ && tq_[tqTail_ % tq_.size()].at <= t0) {
      dx += tq_[tqTail_ % tq_.size()].sec;
      moved = true;
      ++tqTail_;
    }
    if (sh_->strict) {
      // J-159: the travel goes through the wheel travel lane's values (summed, wrapping): the
      // filter takes the difference of two lane values, live and in a replay alike
      const int k = deckParam(idx_, DWheel);
      float n = wheelN_;
      if (wheelLane_) {
        n = wheelLaneN_;
      } else if (moved) {
        wheelRaw_ += dx / kSecondsPerRev;
        wheelRaw_ = wrapTurns(wheelRaw_);
        n = normOf(k, wheelRaw_);
      }
      const double d = wrapTurns(static_cast<double>(valueOfN(k, n)) - static_cast<double>(valueOfN(k, wheelN_)));
      wheelN_ = n;
      dx = d * kSecondsPerRev;
      moved = d != 0.0;
    }
    jogRate(dx, moved, len);
    return jogTrack(len, dx, moved);
  }
  // The tracker over one step in which the hand moved `dx` (seconds of record).
  double jogTrack(int len, double dx, bool moved) {
    double A = 0, B = 0, w = 0;
    jogGains(len, A, B, w);
    const double dt = static_cast<double>(len) / sh_->sr;
    // releasing (and the wheel has stopped): the hand goes on as the motor would move it
    if (jogRelease_ && !moved) dx = releaseTo_ * dt;
    const double eps = jogE_ - dx, u = jogV_ * dt;
    const double u1 = (1.0 - B) * u - A * eps;
    jogE_ = eps + 0.5 * (u + u1);
    jogV_ = u1 / dt;
    if (jogRelease_) {
      // there: the motor has it (live, once the wheel's last ticks are in; a pass's wheel lane
      // has only what reached the tracker, so there it ends as soon as the speed is there)
      if (std::fabs(jogV_ - releaseTo_) <= 1e-5 && (sh_->strict || tqTail_ == tqHead_)) {
        scr_ = ScrOff;
        jogRelease_ = false;
        jogE_ = 0;
        tqTail_ = tqHead_;
        return releaseTo_;
      }
    } else if (!moved && std::fabs(jogV_) < 1e-4 && std::fabs(jogE_) < 1e-5) {
      jogV_ = jogE_ = 0;  // a still hand holds the record still: silence
    }
    return jogV_;
  }

  // -- mouse: a position loop (the hand's travel against the record's)
  // The loop's interval: 16 ms rounded up to whole callbacks of kGlideSec (as an engine
  // with 256-frame callbacks runs it: 17.4 ms), in whole steps.
  double knotLen() const { return std::max(1.0, std::round(std::ceil(0.016 / kGlideSec) * kGlideSec * sh_->sr / kStep)) * kStep; }
  double glideLen() const { return std::max(1.0, std::round(sh_->sr * kGlideSec)); }
  double glideAt(double t) const {
    if (t >= glT1_) return glB_;
    if (t <= glT0_) return glA_;
    return glA_ + (glB_ - glA_) * (t - glT0_) / (glT1_ - glT0_);
  }
  void glideTo(double target, double t0) {
    glA_ = speedNow(), glB_ = target, glT0_ = t0, glT1_ = t0 + glideLen();
  }
  double posGain() const { return 0.3 * feelK(); }
  // The mouse grabs the record: it keeps its speed until the loop has something to go on.
  void handDown() {
    if (!loaded()) return;
    hand_ = true;
    softHand_ = false;
    const double r0 = speedNow();
    const double p = posGain();
    scr_ = ScrPos;
    posTarget_ = 0;
    mouseN_ = normOf(deckParam(idx_, DMouse), 0.0);
    mouseTurns_ = 0;
    // the loop starts settled at the record's speed: the error a P loop needs for it
    pscErr_ = r0 / p;
    posTravel_ = -pscErr_ * unity() * knotLen();
    pscRate_ = r0;
    pscLast_ = 0;
    pscWait_ = 0;
    nextKnot_ = static_cast<double>(sh_->clock) + knotLen();
    glA_ = glB_ = r0, glT0_ = glT1_ = static_cast<double>(sh_->clock);
  }
  void handMove(double turns) {
    if (!(scr_ == ScrPos && hand_)) return;
    if (sh_->strict) {  // (as the mouse travel lane holds it)
      setMouse(normOf(deckParam(idx_, DMouse), wrapTurns(turns)));
      return;
    }
    posTarget_ = turns * srcRate() * kSecondsPerRev;
  }
  // The mouse travel lane's value (normalized, wrapping): the hand's target while it holds the
  // record is the travel summed from the lane's steps.
  void setMouse(double n) {
    const int k = deckParam(idx_, DMouse);
    mouseTurns_ += wrapTurns(static_cast<double>(valueOfN(k, n)) - static_cast<double>(valueOfN(k, mouseN_)));
    mouseN_ = static_cast<float>(n);
    if (scr_ == ScrPos && hand_) posTarget_ = mouseTurns_ * srcRate() * kSecondsPerRev;
  }
  // The mouse lets go: a throw (faster than 2.5x) coasts, anything slower goes back to the motor.
  void handUp() {
    if (!hand_) return;
    if (scr_ != ScrPos) {
      jogLetGo(false);
      return;
    }
    hand_ = false;
    softHand_ = false;
    const double rel = releaseSpeed();
    if (std::fabs(pscRate_) > 2.5) {
      scr_ = ScrThrow;
      thrRate_ = pscRate_;
      thrFloor_ = std::max(std::fabs(rel), 0.01);
    } else {
      scr_ = ScrGlide;
      glideTo(rel, static_cast<double>(sh_->clock));
    }
  }
  // Every ~16 ms: a new speed from the error (smoothed), held when the mouse sent nothing new.
  void posKnot(double t0) {
    const double unit = unity() * knotLen();  // frames the record passes in one knot at its own speed
    const double tgt = posTarget_ / unit;
    bool calc = true;
    if (tgt == pscLast_) {
      pscWait_ += knotLen() / sh_->sr;
      if (pscWait_ < 0.04) {
        calc = false;  // a late mouse update: keep going
      } else if (tgt == 0) {
        pscErr_ = 0;  // never moved: rest
        posTravel_ = 0;
      }
    } else {
      pscWait_ = 0;
      pscLast_ = tgt;
    }
    if (calc) {
      const double e = tgt - posTravel_ / unit;
      // smoothed, except a quick drop of the error (no overshoot on a hard stop)
      const double fast = 0.1 * kGlideSec * sh_->sr / knotLen();
      if (std::fabs(e) - std::fabs(pscErr_) > -fast) pscErr_ += (e - pscErr_) * 0.4;
      else pscErr_ = e;
      pscRate_ = posGain() * pscErr_;
      if (std::fabs(pscRate_) < 0.01) pscRate_ = 0;
      glideTo(pscRate_, t0);
    }
  }
  // One step of any scratch mode but the jog's: the speed at the step's end.
  double scratchStep(int len, double t0) {
    const double t1 = t0 + len;
    switch (scr_) {
      case ScrJog: return jogStep(len, t0);
      case ScrPos:
        while (t0 >= nextKnot_) {
          posKnot(t0);
          nextKnot_ += knotLen();
        }
        return glideAt(t1);
      case ScrThrow: {
        thrRate_ *= std::exp(std::log(thrFloor_ / 100.0) * static_cast<double>(len) / sh_->sr);
        if (std::fabs(thrRate_) < thrFloor_) {
          scr_ = ScrGlide;
          glA_ = thrRate_, glB_ = releaseSpeed(), glT0_ = t0, glT1_ = t0 + glideLen();
          return glideAt(t1);
        }
        return thrRate_;
      }
      case ScrGlide: {
        const double v = glideAt(t1);
        if (t1 >= glT1_) scr_ = ScrOff;
        return v;
      }
      default: return speedNow();
    }
  }
  void scratchOff() {
    scr_ = ScrOff;
    hand_ = softHand_ = jogRelease_ = false;
    jogV_ = jogE_ = 0;
    jogReset();
    tqTail_ = tqHead_;
  }

  void nudgeTicks(int ticks) {
    if (!loaded()) return;
    if (sh_->strict) {  // (J-159: the nudge lane sums them)
      constexpr int span = static_cast<int>(kNudgeSpan);
      nudgeSum_ += ticks;
      while (nudgeSum_ >= span) nudgeSum_ -= 2 * span;
      while (nudgeSum_ < -span) nudgeSum_ += 2 * span;
    }
    if (motor_ && !hand_) {
      nudge_ = std::clamp(nudge_ + ticks * 0.6 / ticksPerRev(), -0.5, 0.5);
    } else if (!motor_) {
      // stopped: the wheel moves the record by hand (it lets go after a moment)
      if (!hand_) jogTouch(true);
      jogTicks(ticks * 0.5 * jogSens());
      softUntil_ = sh_->clock + static_cast<uint64_t>(sh_->sr * 0.08);
    }
  }

  // ---------------------------------------------------------------- inputs
  void input(const InEvent& e) {
    const int c = e.control;
    switch (c) {
      case CtlPlay: buttonPlay(e.down); break;
      case CtlCue: buttonCue(e.down); break;
      case CtlSync:
        if (e.down) {
          toggleCtl(DSync);
          if (!sh_->strict && on(DSync) && motor_) {  // (strict: when the step takes the new value, syncOn)
            motor_ = false;  // re-align the phase now
            startMotor(false);
          }
        }
        break;
      case CtlKeyLock: if (e.down) toggleCtl(DKeyLock); break;
      case CtlSlip: if (e.down) toggleCtl(DSlip); break;
      case CtlVinyl: if (e.down) toggleCtl(DVinyl); break;
      case CtlQuantize: if (e.down) toggleCtl(DQuantize); break;
      case CtlShift: shift_ = e.down; break;
      case CtlTouch:
        if (e.down && on(DVinyl)) jogTouch(false);
        else if (!e.down && scr_ == ScrJog && hand_ && !softHand_) jogLetGo(true);
        break;
      case CtlJogPlatter:
        // touched (or a wheel still turning while it glides back after a release): scratch
        if (scr_ == ScrJog && !softHand_ && (hand_ || jogRelease_)) jogTicks(e.ticks * jogSens());
        else if (sh_->clock >= rimMuteUntil_ || !on(DVinyl)) nudgeTicks(e.ticks);
        break;
      case CtlJogRim:
        if (!hand_ && sh_->clock >= rimMuteUntil_) nudgeTicks(e.ticks);
        break;
      case CtlJogSearch: searchTicks_ += e.ticks; break;
      case CtlPitch: setCtl(DPitch, static_cast<float>(e.value * 2.0 - 1.0)); break;
      case CtlVolume: setCtl(DVolume, e.value); break;
      case CtlEqHigh: setCtl(DEqHigh, static_cast<float>(eqDbOf(e.value))); break;
      case CtlEqMid: setCtl(DEqMid, static_cast<float>(eqDbOf(e.value))); break;
      case CtlEqLow: setCtl(DEqLow, static_cast<float>(eqDbOf(e.value))); break;
      case CtlFilter: setCtl(DFilter, static_cast<float>(std::fabs(e.value - 0.5) < 0.012 ? 0.0 : e.value * 2.0 - 1.0)); break;
      case CtlTrim: setCtl(DTrim, static_cast<float>(e.value * 24.0 - 12.0)); break;
      case CtlLoopIn:
        if (e.down && loaded()) pendingIn_ = quantized(pos_);
        break;
      case CtlLoopOut:
        if (e.down && loaded()) {
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
      case CmdHandDown: handDown(); break;
      case CmdHandMove: handMove(c.v); break;
      case CmdHandUp: handUp(); break;
      case CmdSeek: jump(c.v); break;
      case CmdLoop:
        if (c.v < 0) loopOn_ = false;
        else loopSet(c.v, c.w);
        break;
      default: break;
    }
  }

  // A hot cue key (notes): plays from the cue while held (Gate) or on (Trigger).
  void hotKey(int i, bool down) {
    if (!loaded()) return;
    if (down) {
      double h = hot_[static_cast<size_t>(i)];
      if (h < 0) {  // not set: bar i after the cue
        const int bpb = std::max(1, model().grid.beatsPerBar);
        h = frameAt(model(), beatAt(model(), cue_) + static_cast<double>(i * bpb));
      }
      jump(h);
      noteHot_ = i;
      motor_ = true;
      vel_ = baseSpeed();
      motorSnap_ = true;
    } else if (noteHot_ == i) {
      noteHot_ = -1;
      if (static_cast<int>(std::lround(sh_->p[PNoteMode])) == NoteGate) {
        motor_ = false;
        vel_ = 0;
        motorSnap_ = true;
      }
    }
  }
  void allOff() {
    if (noteHot_ >= 0) hotKey(noteHot_, false);
  }
  void kill() {
    motor_ = false, vel_ = 0, cuePreview_ = false, hotPreview_ = -1, playLatched_ = false, noteHot_ = -1;
    scratchOff();
    censor_ = false, bend_ = 0, nudge_ = 0;
    ghost_.left = 0;
    fresh_ = true;
  }
  bool active() const { return rs() && (motor_ || hand_ || scratching() || std::fabs(vel_) > 1e-9 || ghost_.left > 0 || inTake_); }

  // ---------------------------------------------------------------- performances (J-159)
  // While a pass is captured as a performance (and while one plays back) the decks work
  // in whole steps: the generator applies queued inputs at the step start in a fixed order
  // and every value goes through what its lane holds, so a lane replay matches the pass.
  static double wrapTurns(double x) { return x - 2.0 * kTravelTurns * std::floor((x + kTravelTurns) / (2.0 * kTravelTurns)); }
  // A seek (editor click, search): the deck goes to the record position lane's value.
  // Live, the target becomes a lane value (a new one even when the place is the same, so
  // the lane shows every seek); a replay jumps where the lane changes.
  void seek(double target) {
    if (!loaded() || frames() <= 0) return;
    float n = normOf(deckParam(idx_, DPos), std::clamp(target / frames(), 0.0, 1.0));
    if (n == posN_) n = std::nextafter(n, n < 0.5f ? 1.0f : 0.0f);
    posN_ = n;
    jump(static_cast<double>(valueOfN(deckParam(idx_, DPos), n)) * frames());
  }
  void seekLane(double n) {
    posN_ = static_cast<float>(n);
    if (loaded()) jump(static_cast<double>(valueOfN(deckParam(idx_, DPos), n)) * frames());
  }
  // The lane values a pass starts from (the record position where the record is).
  void perfReset() {
    jogReset();  // (J-197: a pass's replay starts the speed-up afresh: so does the pass)
    posN_ = loaded() && frames() > 0 ? normOf(deckParam(idx_, DPos), std::clamp((jumpPending_ ? jumpTo_ : pos_) / frames(), 0.0, 1.0)) : 0.f;
    wheelRaw_ = 0;
    wheelN_ = normOf(deckParam(idx_, DWheel), 0.0);
    nudgeSum_ = 0;
    mouseTurns_ = scr_ == ScrPos && hand_ ? posTarget_ / (srcRate() * kSecondsPerRev) : 0.0;
    mouseN_ = normOf(deckParam(idx_, DMouse), wrapTurns(mouseTurns_));
  }
  float laneOut(int d) const {
    switch (d) {
      case DPos: return posN_;
      case DMouse: return mouseN_;
      case DWheel: return wheelN_;
      default: return normOf(deckParam(idx_, DNudge), static_cast<double>(nudgeSum_));
    }
  }

  // The deck's state where a pass starts (restored by the performance's start note):
  // times relative to now; the key lock engine restarts there anyway.
  template <class F>
  void snapFields(F&& f) {
    const double now = static_cast<double>(sh_->clock);
    double v;
    f(pos_), f(vel_), f(prevRate_), f(tan_), f(jumpTo_), f(cue_), f(savedIn_), f(savedOut_), f(loopIn_), f(loopOut_), f(pendingIn_), f(slipPos_);
    for (auto& h : hot_) f(h);
    f(jogE_), f(jogV_), f(releaseTo_), f(posTarget_), f(posTravel_), f(pscErr_), f(pscRate_), f(pscLast_), f(pscWait_);
    v = nextKnot_ - now, f(v), nextKnot_ = v + now;
    f(glA_), f(glB_);
    v = glT0_ - now, f(v), glT0_ = v + now;
    v = glT1_ - now, f(v), glT1_ = v + now;
    f(thrRate_), f(thrFloor_), f(nudge_), f(bend_);
    v = static_cast<double>(softUntil_) - now, f(v), softUntil_ = static_cast<uint64_t>(std::max(0.0, v + now));
    v = static_cast<double>(rimMuteUntil_) - now, f(v), rimMuteUntil_ = static_cast<uint64_t>(std::max(0.0, v + now));
    auto fb = [&](bool& b) {
      double x = b ? 1 : 0;
      f(x);
      b = x != 0;
    };
    auto fi = [&](int& i) {
      double x = i;
      f(x);
      i = static_cast<int>(x);
    };
    fb(motor_), fb(hand_), fb(softHand_), fb(censor_), fb(jogRelease_), fb(fresh_), fb(jumpPending_), fb(cuePreview_), fb(playLatched_), fb(loopOn_),
        fb(slipping_), fb(keyOn_);
    fi(hotPreview_), fi(noteHot_), fi(searchTicks_), fi(keyCur_), fi(keyGhost_);
    double sc = scr_;
    f(sc);
    scr_ = static_cast<uint8_t>(sc);
    double lg = lastGain_;
    f(lg);
    lastGain_ = static_cast<float>(lg);
    f(ghost_.pos), f(ghost_.rate);
    fi(ghost_.left);
    for (auto& a : eq_.lo)
      for (auto& q : a) f(q.ic1), f(q.ic2);
    for (auto& a : eq_.hi)
      for (auto& q : a) f(q.ic1), f(q.ic2);
    for (double& g : eqGain_) f(g);
    for (auto& q : filt_) f(q.ic1), f(q.ic2);
    f(wet_);
    double pn = posN_;
    f(pn);
    posN_ = static_cast<float>(pn);
    double mn = mouseN_;
    f(mn);
    mouseN_ = static_cast<float>(mn);
    f(mouseTurns_);
  }
  // Restored: the rest of what a step carries (the next one starts fresh where the pass did).
  void perfRestored() {
    wheelRaw_ = 0;
    wheelN_ = normOf(deckParam(idx_, DWheel), 0.0);
    nudgeSum_ = 0;
    tqTail_ = tqHead_;
    jogReset();
    keyRestart_ = keyOn_;
    marksChanged();
  }

  // ---------------------------------------------------------------- passes as state (J-163)
  // The scale of this deck's position lanes (its own track).
  PosScale scale() const { return src_ ? PosScale(static_cast<double>(src_->frames()), src_->sampleRate) : PosScale(); }
  bool touchOn() const { return scr_ == ScrJog && hand_ && !softHand_; }  // a hand on the wheel's top
  bool mouseOn() const { return scr_ == ScrPos && hand_; }                 // the mouse holds the record
  int bendState() const { return bend_ < 0 ? BendDown : bend_ > 0 ? BendUp : BendOff; }
  // A pass's start state was restored here (the cues stay as they are now).
  void passRestored() {
    wheelRaw_ = 0;
    wheelN_ = normOf(deckParam(idx_, DWheel), 0.0);
    nudgeSum_ = 0;
    tqTail_ = tqHead_;
    jogReset();
    keyRestart_ = keyOn_;
  }
  // One step of a pass's state lanes, where the hands acted when it was recorded (the step
  // start, before the travel lanes): the motor, the loop, reverse, bend, the wheel touch, the
  // mouse hand, then the record's jumps. Everything between them is the deck's own physics,
  // as it was live, so an unedited pass plays sample for sample.
  void drive(const DriveStep& s) {
    if (!loaded()) return;
    if (s.chase) {
      chase(s);
      return;
    }
    if (s.play >= 0 && ((s.play > 0) != motor_ || s.instant)) {
      motor_ = s.play > 0;
      if (s.instant) vel_ = motor_ ? baseSpeed() : 0.0;
    }
    if (s.hasIn) loopIn_ = s.loopIn;
    if (s.hasOut) loopOut_ = s.loopOut;
    if (s.loop >= 0) loopOn_ = s.loop > 0;
    if (s.rev >= 0 && (s.rev > 0) != censor_) {
      censor_ = s.rev > 0;
      if (censor_) vel_ = -std::fabs(vel_);
      else if (motor_) vel_ = std::fabs(vel_);
    }
    if (s.bend >= 0) bend_ = s.bend == BendDown ? -0.04 : s.bend == BendUp ? 0.04 : 0.0;
    if (s.touch >= 0) {
      if (s.touch > 0 && !touchOn()) jogTouch(false);
      else if (s.touch == 0 && touchOn()) jogLetGo(true);
    }
    if (s.hand >= 0) {
      if (s.hand > 0 && !mouseOn()) handDown();
      else if (s.hand == 0 && mouseOn()) handUp();
    }
    if (s.jump) {
      // a loop's wrap: the deck makes it itself in the step (the same place)
      bool wrap = false;
      if (loopOn_ && loopOut_ > loopIn_ && !jumpPending_) {
        const double L = loopOut_ - loopIn_;
        wrap = s.jumpTo == q256(pos_ - L) || s.jumpTo == q256(pos_ + L);
      }
      if (!wrap) jump(s.jumpTo);
    } else if (s.check && !hand_ && !scratching() && std::fabs((jumpPending_ ? jumpTo_ : pos_) - s.lanePos) > 4.0) {
      jump(s.lanePos);  // the lane was edited: the record goes where it says
    }
  }
  // Picks a pass up in the middle: the deck takes the lanes' state at once.
  void chase(const DriveStep& s) {
    noteHot_ = -1, cuePreview_ = false, hotPreview_ = -1, playLatched_ = false, slipping_ = false;
    scratchOff();
    searchTicks_ = 0;
    if (s.hasIn) loopIn_ = s.loopIn;
    if (s.hasOut) loopOut_ = s.loopOut;
    loopOn_ = s.loop > 0 && loopOut_ > loopIn_;
    censor_ = s.rev > 0;
    bend_ = s.bend == BendDown ? -0.04 : s.bend == BendUp ? 0.04 : 0.0;
    nudge_ = 0;
    if (s.play >= 0) motor_ = s.play > 0;
    if (s.hasPos) {
      // (a record that was sounding crossfades to the place; a silent one is put there)
      if (std::fabs(vel_) > 1e-9 || std::fabs(prevRate_) > 1e-9 || ghost_.left > 0) {
        jump(s.pos);
      } else {
        pos_ = s.pos;
        jumpPending_ = false;
      }
    }
    // the speed: the lane's own (a hand, a spin-up or a brake), the motor's on a steady run
    double v = s.hasPos ? s.vel : 0.0;
    if (s.touch <= 0 && s.hand <= 0 && motor_) {
      double target = baseSpeed() * (1.0 + bend_);
      if (censor_) target = -std::fabs(target);
      if (!s.hasPos || std::fabs(v - target) <= 1e-6 * std::fabs(target) + 1e-9) v = target;
    }
    vel_ = prevRate_ = tan_ = v;
    fresh_ = true;
    lastGain_ = gq(faderGain());
    keyRestart_ = keyOn_;
    if (s.touch > 0 && on(DVinyl)) {
      jogTouch(false);
      if (s.warmN > 0 && s.warmDx) {  // (the filter runs over the hand's last steps: its speed and lag as they were)
        // (from the touch, as it caught the record then: the tracker's state comes out as it was)
        jogReset();
        jogV_ = s.warmV / std::max(1e-9, unity());
        jogCatch();
        for (int i = 0; i < s.warmN; ++i) {
          const double dx = s.warmDx[i];
          jogRate(dx, dx != 0.0, kStep);
          jogTrack(kStep, dx, dx != 0.0);
        }
        vel_ = prevRate_ = tan_ = jogV_ * unity();  // (the speed this step starts from)
      }
    } else if (s.hand > 0) {
      handDown();
    }
  }

  // ---------------------------------------------------------------- takes
  // The deck plays its stream of a take (null: it stays silent for the take).
  void startTake(const TakeStream* s, const Sample* src) {
    inTake_ = true;
    fresh_ = true;  // (the take's first step is straight, as it was when it was recorded)
    stream_ = s && s->on && src ? s : nullptr;
    takeSrc_ = stream_ ? src : nullptr;
    scratchOff();
    cuePreview_ = false, hotPreview_ = -1, noteHot_ = -1, censor_ = false;
    loopOn_ = false;
    ghost_.left = 0;
    keyOn_ = false;
    if (stream_) {
      jumpTo_ = stream_->startPos;
      jumpPending_ = true;
      vel_ = stream_->startRate;
    } else {
      motor_ = false, vel_ = 0, jumpPending_ = false;
    }
  }
  // Re-point the take after an aux change (false: it is gone).
  void repointTake(const TakeStream* s, const Sample* src) {
    if (!inTake_) return;
    stream_ = s && s->on && src && src == takeSrc_ ? s : nullptr;
    if (!stream_) takeSrc_ = nullptr;
  }
  // The take ended (it ran out, or something took over): the deck carries on
  // from where the take left the record when it played this deck's own track.
  void endTake(bool ranOut) {
    if (!inTake_) return;
    const bool own = takeSrc_ && takeSrc_ == src_;
    inTake_ = false;
    stream_ = nullptr;
    takeSrc_ = nullptr;
    if (own) {
      motor_ = takeMotor_;
      if (!ranOut) vel_ = prevRate_;  // (a take that ran out left vel_ at its last speed)
    } else {
      motor_ = false;
      vel_ = 0;
      ghost_.left = 0;
      keyOn_ = false;
      pos_ = src_ ? cue_ : 0;
      jumpPending_ = false;
    }
  }

  // ---------------------------------------------------------------- the step
  static double q256(double x) { return std::round(x * 256.0) / 256.0; }
  static float gq(double g) { return static_cast<float>(std::round(g * 4096.0) / 4096.0); }

  // Channel fader x crossfader.
  double faderGain() const {
    const double v = std::clamp(static_cast<double>(P(DVolume)), 0.0, 1.0);
    double g = v;
    switch (static_cast<int>(std::lround(sh_->p[PFaderCurve]))) {
      case FcSmooth: g = v * v; break;
      case FcSharp: g = std::min(1.0, v * 6.0); break;
      default: break;
    }
    const int side = ipar(DXfSide);
    if (side != XfThru) {
      double x = std::clamp(static_cast<double>(sh_->p[PXfader]), 0.0, 1.0);
      if (sh_->p[PXfReverse] >= 0.5f) x = 1.0 - x;
      if (side == XfA) x = 1.0 - x;  // A is loud at 0
      const double c = std::clamp(static_cast<double>(sh_->p[PXfCurve]), 0.0, 1.0);
      const double k = sh_->p[PXfCut] >= 0.5f ? 400.0 : 1.0 + c * c * 40.0;
      g *= std::sin(std::min(1.0, x * k) * kPi * 0.5);
    }
    return g;
  }
  // Level of a record at `r` times its own speed: a cartridge's output follows
  // the stylus speed (a record crawling past is faint, one standing still is
  // silent): log(1 + 4 r) / log(5), the curve Mixxx measured on a real record
  // (-6 dB at 0.3x, 0 dB at 1x), not above 0 dB.
  static double vinylGain(double r) {
    r = std::fabs(r);
    return r >= 1.0 ? 1.0 : std::log1p(4.0 * r) * kInvLog5;
  }

  // Before the step: what the other deck may lock to.
  void preStep() { startBeat_ = gridOk() ? beatAt(model(), jumpPending_ ? jumpTo_ : pos_) : 0.0; }

  // Plans the next step of `len` samples. Replay: `takeT` / `adv` / `exact` place it in the take.
  void computeStep(int len, double takeT, double adv, double stepOut, bool exact) {
    plan_.silent = !rs();
    plan_.src = rs();
    plan_.len = len;
    pointOk_ = false;
    if (!rs()) return;
    const double fLen = static_cast<double>(len);
    const double x0 = pos_;
    const double v0 = vel_;
    double start = x0, x1 = x0, v1 = v0;
    bool jumped = false;
    uint8_t flags = 0;
    float gain = gq(faderGain());
    const uint64_t clock = sh_->clock;

    // soft hand (wheel while stopped) lets go
    if (softHand_ && clock >= softUntil_) jogLetGo(false);
    // search: whole jumps
    if (searchTicks_ != 0) {
      jump(pos_ + searchTicks_ * srcRate() * 0.2);
      searchTicks_ = 0;
    }
    // slip: the ghost position keeps time while something holds the record
    // (J-163: a deck a pass drives does not slip: the pass's position lane has the slip's return)
    const bool slipNow = !inTake_ && !driven_ && on(DSlip) && (hand_ || scratching() || censor_ || loopOn_ || hotPreview_ >= 0);
    if (driven_) slipping_ = false;
    if (slipNow && !slipping_) slipping_ = true, slipPos_ = x0;
    if (!slipNow && slipping_) {
      slipping_ = false;
      if (motor_) jump(slipPos_);
    }

    const bool replay = inTake_;
    if (replay) {
      // ---- replay
      const TakeStream& t = *stream_;
      const double tEnd = takeT + adv;
      if (jumpPending_) {  // the take's first step lands on its start
        start = jumpTo_;
        jumped = std::fabs(jumpTo_ - x0) > 0.5;
        jumpPending_ = false;
      }
      if (exact) {
        const auto i = static_cast<size_t>(std::llround(takeT));
        if (i < t.pts.size()) {
          const TakePoint& p = t.pts[i];
          if (p.flags & TfJump) start = p.from, jumped = true;
          x1 = p.pos;
          gain = p.gain;
          flags = p.flags;
        }
      } else if (!t.pts.empty()) {
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
        for (double k = std::floor(takeT) + 1; k <= tEnd && k < static_cast<double>(t.pts.size()); k += 1)
          if (t.pts[static_cast<size_t>(k)].flags & TfJump) {
            start = t.pts[static_cast<size_t>(k)].from;
            jumped = true;
            flags |= TfJump;
          }
      }
      (void)stepOut;
      takeMotor_ = (flags & TfMotor) != 0;
      v1 = (x1 - start) / fLen;
      motor_ = takeMotor_;
    } else {
      // ---- live physics
      if (jumpPending_) {
        jumped = std::fabs(jumpTo_ - x0) > 1e-6;
        // (J-163: on the 1/256 frame grid before the step moves on, as a position lane holds it)
        start = jumped ? q256(jumpTo_) : jumpTo_;
        jumpPending_ = false;
      }
      if (scratching()) {
        // scratching: the scratch speed at the step's end; the speed ramps to it
        // across the step (the record's travel is what the mouse loop compares)
        v1 = scratchStep(len, static_cast<double>(clock)) * unity();
        x1 = start + 0.5 * (v0 + v1) * fLen;
        flags |= TfHand;
      } else {
        const double base = baseSpeed();
        double target = motor_ ? base * (1.0 + bend_ + nudge_) : 0.0;
        if (censor_) target = -std::fabs(target);
        // phase lock to what the deck follows (sync, motor running steadily)
        // (not while a nudge or bend moves the phase on purpose)
        double want = 0;
        if (motor_ && on(DSync) && !censor_ && bend_ == 0 && nudge_ == 0 && std::fabs(v0 - target) <= 0.05 * std::fabs(target) && noteHot_ < 0 &&
            !loopOn_ && leaderPhase(static_cast<double>(clock - sh_->blockClock), want)) {
          const double db = beatAt(model(), start);
          double err = (db - std::floor(db)) - want;
          if (err > 0.5) err -= 1;
          if (err < -0.5) err += 1;
          const double corr = std::clamp(-err * 0.6, -0.04, 0.04);
          target *= 1.0 + corr;
        }
        const double t = motor_ && std::fabs(target) >= std::fabs(v0) ? P(DStartTime) : P(DStopTime);
        const double slope = t > 1e-4 ? std::max(unity(), std::fabs(base)) * fLen / (static_cast<double>(t) * sh_->sr) : 1e9;
        v1 = v0 + std::clamp(target - v0, -slope, slope);
        if (!motor_ && std::fabs(v1) < 1e-5 * unity()) v1 = 0;
        x1 = start + 0.5 * (v0 + v1) * fLen;
        if (motor_) flags |= TfMotor;
        // nudge decays (about 120 ms)
        nudge_ *= std::exp(-fLen / (0.12 * sh_->sr));
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
      if (!scratching()) v1 = 0, motor_ = motor_ && x1 < lim;
    }
    start = q256(start);
    x1 = q256(x1);
    if (!replay && (flags & TfHand)) posTravel_ += x1 - start;
    if (jumped) flags |= TfJump;
    const double rate = (x1 - start) / fLen;

    // key lock: steady motor play between 0.5x and 1.9x
    const double rel = rate / std::max(1e-9, unity());
    // (a take replays its own choice, its last step included)
    bool wantKey = false;
    if (replay) wantKey = sh_->pool && (flags & TfKeyLock);
    else
      wantKey = sh_->pool && on(DKeyLock) && (flags & TfMotor) && !(flags & TfHand) && rel > 0.5 && rel < 1.9 && !censor_ &&
                std::fabs(v1 - v0) < 2e-3 * unity();
    if (wantKey) flags |= TfKeyLock;
    else flags &= static_cast<uint8_t>(~TfKeyLock);

    // the path inside the step: its speed starts where the last step's ended
    // and ends on the line through the last two step means (a steady change of
    // speed is then a straight ramp, with no bump; after a jump, at a fresh
    // start and on a take's first step: straight). Positions alone decide it,
    // so a take replays the same path.
    const bool straight = jumped || fresh_;
    const double m0 = straight ? rate : tan_;
    const double m1 = straight ? rate : rate + 0.5 * (rate - prevRate_);
    planStep(len, x0, start, x1, rate, v0, v1, jumped, wantKey, gain);
    plan_.m0 = m0;
    plan_.m1 = m1;
    plan_.lin = m0 == rate && m1 == rate;
    plan_.hand = !replay && (flags & TfHand);
    fresh_ = false;

    // state for the next step
    vel_ = v1;
    prevRate_ = rate;
    tan_ = m1;
    pos_ = x1;
    if (slipping_ && motor_) slipPos_ += baseSpeed() * fLen;
    lastGain_ = gain;
    pt_.pos = x1;
    pt_.from = jumped ? start : 0;
    pt_.gain = gain;
    pt_.flags = flags;
    pointOk_ = true;
  }

  // Settings for the step just planned (ghosts, key lock engine, EQ / filter).
  void planStep(int len, double x0, double start, double x1, double rate, double v0, double v1, bool jumped, bool keyMode, float gain) {
    Plan& Pl = plan_;
    const double sr = sh_->sr;
    Pl.len = len, Pl.start = start, Pl.x1 = x1, Pl.rate = rate, Pl.v0 = v0, Pl.v1 = v1, Pl.key = keyMode;
    Pl.g0 = lastGain_, Pl.g1 = gain;
    Pl.inv = 1.0 / std::max(1e-9, unity());
    KeyPool* pool = sh_->pool;
    // a jump: the old motion carries on as a ghost and fades out
    if (jumped) {
      if (keyOn_ && pool) {
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
        Reader& rd = pool->rd[idx_][keyCur_];
        rd.src = rs();
        // primed with the audio leading into the start point: the first block is already settled
        const int pre = pool->v[idx_][keyCur_].prerollFor(rate);
        rd.cursor = static_cast<int64_t>(std::floor(start)) - pre;
        pool->v[idx_][keyCur_].start(readFrames, &rd, rate, srcRate() / sr, pre);
        keyOn_ = true;
        keyRestart_ = false;
      }
    } else if (keyOn_) {
      // back to the vinyl path: the key lock engine fades out as the ghost
      keyGhost_ = keyCur_;
      ghost_.left = kFade;
      keyOn_ = false;
    }
    Pl.ghostRate = std::fabs(prevRate_) > 1e-9 ? std::fabs(prevRate_) : unity();
    // EQ / filter targets
    const float eh = P(DEqHigh), em = P(DEqMid), el = P(DEqLow);
    Pl.eqH = eh <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(eh);
    Pl.eqM = em <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(em);
    Pl.eqL = el <= kEqKillDb + 1e-3 ? 0.0 : dbToGain(el);
    Pl.trim = dbToGain(P(DTrim));
    const double fz = P(DFilter);
    Pl.wetT = std::clamp((std::fabs(fz) - 0.02) / 0.06, 0.0, 1.0);
    double fc = 20000;
    if (fz < 0) fc = 20000.0 * std::pow(60.0 / 20000.0, std::min(1.0, -fz));
    else if (fz > 0) fc = 20.0 * std::pow(9000.0 / 20.0, std::min(1.0, fz));
    Pl.fg = std::tan(kPi * std::min(fc, sr * 0.45) / sr);
    Pl.fk = 2.0 - 1.85 * std::clamp(static_cast<double>(P(DFilterRes)), 0.0, 1.0);
    Pl.hp = fz > 0;
    Pl.smooth = 1.0 - std::exp(-1.0 / (0.004 * sr));
  }

  // A note lands inside a step: the step ends at `at` and the next one starts there.
  void cut(int at) {
    if (plan_.silent || at <= 0 || at >= plan_.len) return;
    const Plan& Pl = plan_;
    const double u = static_cast<double>(at) / Pl.len;
    double spd = Pl.rate;
    pos_ = pathAt(Pl, u, spd);
    tan_ = spd;
    if (Pl.hand) posTravel_ -= Pl.x1 - pos_;  // (the mouse loop counts what the record really travelled)
    if (!inTake_) vel_ = Pl.v0 + (Pl.v1 - Pl.v0) * u;
    lastGain_ = static_cast<float>(Pl.g0 + (Pl.g1 - Pl.g0) * u);
  }

  // Renders samples [from, from + count) of the planned step into L / R (adds).
  void runStep(float* L, float* R, int from, int count) {
    if (plan_.silent) return;
    const Plan& Pl = plan_;
    const Sample& s = *Pl.src;
    const double ratio = s.sampleRate / sh_->sr;
    const auto n = static_cast<int64_t>(s.frames());
    const float* xs[2] = {s.left.data(), s.right.data()};
    const pitch::SincTable& tab = pitch::table(pitch::Quality::Realtime);
    const double fLen = static_cast<double>(Pl.len);
    KeyPool* pool = sh_->pool;
    float kl[2][kStep], kg[2][kStep];
    if (Pl.key) {
      float* out[2] = {kl[0], kl[1]};
      pool->v[idx_][keyCur_].render(out, count, Pl.rate, ratio, readFrames, &pool->rd[idx_][keyCur_]);
    }
    const bool keyGhostNow = keyGhost_ >= 0 && ghost_.left > 0 && pool;
    if (keyGhostNow) {
      float* out[2] = {kg[0], kg[1]};
      pool->v[idx_][keyGhost_].render(out, count, Pl.ghostRate, ratio, readFrames, &pool->rd[idx_][keyGhost_]);
    }
    const bool eqFlat = Pl.eqH == 1.0 && Pl.eqM == 1.0 && Pl.eqL == 1.0 && eqGain_[0] == 1.0 && eqGain_[1] == 1.0 && eqGain_[2] == 1.0;
    for (int c = 0; c < count; ++c) {
      const int j = from + c;
      const double u = static_cast<double>(j) / fLen;
      double l = 0, r = 0;
      if (Pl.key) {
        l = kl[0][c], r = kl[1][c];
      } else {
        double spd = Pl.rate;
        const double p = pathAt(Pl, u, spd);
        if (pitch::exactFrame(p, spd)) {  // on a whole frame (at most normal speed): the file itself
          const auto k = static_cast<int64_t>(std::floor(p + 0.5));
          const bool in = k >= 0 && k < n;
          l = in ? xs[0][k] : 0.0, r = in ? xs[1][k] : 0.0;
        } else {
          float o[2];
          pitch::readArrays<2>(xs, n, p, std::max(std::fabs(spd), 1e-6), tab, o);
          l = o[0], r = o[1];
        }
        // the level follows the speed at this very sample (a turn passes through silence)
        const double sg = vinylGain(spd * Pl.inv);
        l *= sg, r *= sg;
      }
      if (ghost_.left > 0) {
        double gl = 0, gr = 0;
        if (keyGhostNow) {
          gl = kg[0][c], gr = kg[1][c];
        } else {
          float o[2];
          pitch::readArrays<2>(xs, n, ghost_.pos, std::max(std::fabs(ghost_.rate), 1e-6), tab, o);
          const double gsg = vinylGain(ghost_.rate * Pl.inv);
          gl = o[0] * gsg, gr = o[1] * gsg;
          ghost_.pos += ghost_.rate;
        }
        const double w = static_cast<double>(ghost_.left) / kFade;  // 1 -> 0
        const double wg = std::sin(w * kPi * 0.5), wm = std::cos(w * kPi * 0.5);
        l = l * wm + gl * wg;
        r = r * wm + gr * wg;
        if (--ghost_.left == 0) keyGhost_ = -1;
      }
      l *= Pl.trim, r *= Pl.trim;
      // isolator EQ (the bands sum back to the input exactly; flat skips it)
      for (int b = 0; b < 3; ++b) {
        const double tgt = b == 0 ? Pl.eqL : (b == 1 ? Pl.eqM : Pl.eqH);
        eqGain_[b] += (tgt - eqGain_[b]) * Pl.smooth;
        if (std::fabs(eqGain_[b] - tgt) < 1e-7) eqGain_[b] = tgt;
      }
      double bl, bm, bh;
      eq_.bands(0, l, bl, bm, bh);
      if (!eqFlat) l = bl * eqGain_[0] + bm * eqGain_[1] + bh * eqGain_[2];
      eq_.bands(1, r, bl, bm, bh);
      if (!eqFlat) r = bl * eqGain_[0] + bm * eqGain_[1] + bh * eqGain_[2];
      // filter (low-pass left of centre, high-pass right), faded in past the centre
      wet_ += (Pl.wetT - wet_) * Pl.smooth;
      if (wet_ < 1e-6 && Pl.wetT == 0.0) wet_ = 0;
      if (wet_ > 0) {
        double a, b, cc;
        filt_[0].run(l, Pl.fg, Pl.fk, a, b, cc);
        const double fl = Pl.hp ? cc : a;
        filt_[1].run(r, Pl.fg, Pl.fk, a, b, cc);
        const double fr = Pl.hp ? cc : a;
        l += (fl - l) * wet_;
        r += (fr - r) * wet_;
      } else {
        filt_[0].reset();
        filt_[1].reset();
      }
      pre_ = std::max(pre_, static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
      const double g = Pl.g0 + (Pl.g1 - Pl.g0) * (static_cast<double>(j + 1) / fLen);
      const auto ol = static_cast<float>(l * g), orr = static_cast<float>(r * g);
      L[c] += ol;
      R[c] += orr;
      peak_ = std::max(peak_, std::max(std::fabs(ol), std::fabs(orr)));
    }
  }

  void setEqRate(double sr) {
    if (sr != eqRate_) {
      eq_.setup(sr);
      eqRate_ = sr;
    }
  }

  void publish(bool capturing, int takeKey) {
    const float peak = peak_, pre = pre_;
    peak_ = pre_ = 0;
    pubLoaded_.store(loaded(), std::memory_order_relaxed);
    pubPos_.store(pos_, std::memory_order_relaxed);
    pubSpeed_.store(prevRate_ / std::max(1e-9, unity()), std::memory_order_relaxed);
    pubMotor_.store(motor_, std::memory_order_relaxed);
    pubHand_.store(hand_ && !softHand_, std::memory_order_relaxed);
    pubWheel_.store(wheelTicks_, std::memory_order_relaxed);
    pubSlipping_.store(slipping_, std::memory_order_relaxed);
    pubSlipPos_.store(slipping_ ? slipPos_ : -1, std::memory_order_relaxed);
    pubLoopOn_.store(loopOn_, std::memory_order_relaxed);
    pubLoopIn_.store(loopOn_ ? loopIn_ : savedIn_, std::memory_order_relaxed);
    pubLoopOut_.store(loopOn_ ? loopOut_ : savedOut_, std::memory_order_relaxed);
    pubSavedIn_.store(savedIn_, std::memory_order_relaxed);
    pubSavedOut_.store(savedOut_, std::memory_order_relaxed);
    pubCue_.store(cue_, std::memory_order_relaxed);
    pubCueLit_.store(cuePreview_ || (!motor_ && loaded() && std::fabs(pos_ - cue_) < srcRate() * 0.01), std::memory_order_relaxed);
    for (int k = 0; k < kHotCues; ++k) pubHot_[static_cast<size_t>(k)].store(hot_[static_cast<size_t>(k)], std::memory_order_relaxed);
    uint32_t f = 0;
    if (on(DSync)) f |= 1;
    if (on(DKeyLock)) f |= 2;
    if (on(DSlip)) f |= 4;
    if (on(DVinyl)) f |= 8;
    if (on(DQuantize)) f |= 16;
    if (keyOn_) f |= 32;
    if (shift_) f |= 64;
    if (capturing) f |= 128;
    if (on(DSync) && leader_ == kLeadDeck) f |= 256;
    pubFlags_.store(f, std::memory_order_relaxed);
    pubTempo_.store(effectiveBpm(), std::memory_order_relaxed);
    pubBeat_.store(gridOk() ? beatAt(model(), pos_) : 0.0, std::memory_order_relaxed);
    pubTake_.store(inTake_ ? takeKey : -1, std::memory_order_relaxed);
    pubLevel_.store(peak, std::memory_order_relaxed);
    pubPre_.store(pre, std::memory_order_relaxed);
  }
  void countWheel(int ticks) { wheelTicks_ += static_cast<uint64_t>(std::abs(ticks)); }
  void view(LiveView& v) const {
    v.valid = true;
    v.loaded = pubLoaded_.load(std::memory_order_relaxed);
    v.pos = pubPos_.load(std::memory_order_relaxed);
    v.speed = pubSpeed_.load(std::memory_order_relaxed);
    v.playing = pubMotor_.load(std::memory_order_relaxed);
    v.touched = pubHand_.load(std::memory_order_relaxed);
    v.wheelTicks = pubWheel_.load(std::memory_order_relaxed);
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
    v.following = f & 256;
    v.tempo = pubTempo_.load(std::memory_order_relaxed);
    v.beat = pubBeat_.load(std::memory_order_relaxed);
    v.take = pubTake_.load(std::memory_order_relaxed);
    v.level = pubLevel_.load(std::memory_order_relaxed);
    v.pre = pubPre_.load(std::memory_order_relaxed);
  }
  void marksOut(Marks& m) const {
    m.seq = marksSeq_.load(std::memory_order_acquire);
    m.cue = pubCue_.load(std::memory_order_relaxed);
    for (int k = 0; k < kHotCues; ++k) m.hot[static_cast<size_t>(k)] = pubHot_[static_cast<size_t>(k)].load(std::memory_order_relaxed);
    m.loopIn = pubSavedIn_.load(std::memory_order_relaxed);
    m.loopOut = pubSavedOut_.load(std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------- state (the generator reads some)
  static constexpr int kLeadNone = -1, kLeadProject = 0, kLeadDeck = 1;
  int idx_ = 0;
  Shared* sh_ = nullptr;
  const Deck* other_ = nullptr;
  const Sample* src_ = nullptr;
  const DeckModel* dm_ = nullptr;
  int fileIdx_ = -1;
  int leader_ = kLeadNone;
  double syncBpm_ = 0, startBeat_ = 0;

  double pos_ = 0, vel_ = 0, prevRate_ = 0;
  bool motor_ = false, hand_ = false, softHand_ = false, censor_ = false, shift_ = false;
  // scratching (speeds in times the record's own speed)
  enum Scr : uint8_t { ScrOff = 0, ScrJog, ScrPos, ScrThrow, ScrGlide };
  uint8_t scr_ = ScrOff;
  // jog (J-197): the tracker (e: the record's place less the hand's, seconds of record; v: its
  // speed), its speed-up (the last moving steps: their gaps (s) and ticks), the wheel's poles
  // (logs per millisecond, cached), the ticks waiting for their time
  double jogE_ = 0, jogV_ = 0, jogS_ = 1, jogSince_ = kJogTau0;
  std::array<double, kJogHist> jogGap_{}, jogTk_{};
  int jogHistN_ = 0;
  double jpA_ = -1, jpB_ = -1;
  std::array<std::complex<double>, 2> jpLog_{};
  std::array<bool, 2> jpZero_{};
  bool jogRelease_ = false;
  double releaseTo_ = 0;
  struct Tick {
    double at = 0, sec = 0;  // sample time, seconds of record
  };
  std::array<Tick, 512> tq_{};
  size_t tqHead_ = 0, tqTail_ = 0;
  // mouse: target and record travel since the grab (frames), the loop's state, the next 16 ms knot
  double posTarget_ = 0, posTravel_ = 0, pscErr_ = 0, pscRate_ = 0, pscLast_ = 0, pscWait_ = 0, nextKnot_ = 0;
  // a speed ramp (from glA_ at glT0_ to glB_ at glT1_, sample times) and a throw
  double glA_ = 0, glB_ = 0, glT0_ = 0, glT1_ = 0, thrRate_ = 0, thrFloor_ = 0.01;
  double tan_ = 0;     // speed at the start of the next step
  bool fresh_ = true;  // the next step starts straight
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

  // performances (J-159): the lane values of this deck (live: what the pass records; replay:
  // what the lanes said last), the wheel lane playing this step
  float posN_ = 0, mouseN_ = 0.5f, wheelN_ = 0.5f;
  double mouseTurns_ = 0;  // the mouse hand's travel, summed from the lane's steps
  double wheelRaw_ = 0;
  int nudgeSum_ = 0;
  bool wheelLane_ = false;
  float wheelLaneN_ = 0.5f;
  // J-163: the motor started / stopped at once since the last step (a pass's play lane says so);
  // a recorded pass's state lanes drive the deck (its own hands are ignored meanwhile)
  bool motorSnap_ = false, driven_ = false;

  // takes
  bool inTake_ = false, takeMotor_ = false;
  const TakeStream* stream_ = nullptr;
  const Sample* takeSrc_ = nullptr;
  TakePoint pt_;
  bool pointOk_ = false;
  bool keyRestart_ = false, keyOn_ = false;
  float lastGain_ = 1;

 private:
  struct Plan {
    bool silent = true, key = false, hand = false, hp = false;
    int len = kStep;
    double start = 0, x1 = 0, rate = 0, v0 = 0, v1 = 0, inv = 1, ghostRate = 1;
    double m0 = 0, m1 = 0;  // speed at the step's start and end
    bool lin = true;        // m0 == m1 == rate: a straight path
    double g0 = 0, g1 = 0;
    const Sample* src = nullptr;  // the file the step reads (a take that ends mid-step keeps it to the step's end)
    double eqH = 1, eqM = 1, eqL = 1, trim = 1, wetT = 0, fg = 0.5, fk = 1.4, smooth = 0.01;
  };
  // Read position at `u` (0..1) of a planned step and the speed there (frames per
  // sample): a cubic Hermite path from the start speed m0 to the end speed m1.
  static double pathAt(const Plan& Pl, double u, double& spd) {
    const double c = Pl.x1 - Pl.start;
    if (Pl.lin) {
      spd = Pl.rate;
      return Pl.start + c * u;
    }
    const double L = static_cast<double>(Pl.len), u2 = u * u, u3 = u2 * u;
    spd = Pl.m0 * (3 * u2 - 4 * u + 1) + Pl.rate * (6 * u - 6 * u2) + Pl.m1 * (3 * u2 - 2 * u);
    return Pl.start + (u3 - 2 * u2 + u) * L * Pl.m0 + (3 * u2 - 2 * u3) * c + (u3 - u2) * L * Pl.m1;
  }
  Plan plan_;
  float peak_ = 0, pre_ = 0;
  Ghost ghost_;
  Isolator eq_;
  double eqRate_ = 0;
  double eqGain_[3] = {1, 1, 1};
  Svf filt_[2];
  double wet_ = 0;
  int keyCur_ = 0, keyGhost_ = -1;

  // published
  std::atomic<bool> pubLoaded_{false}, pubMotor_{false}, pubHand_{false}, pubSlipping_{false}, pubLoopOn_{false}, pubCueLit_{false};
  std::atomic<double> pubPos_{0}, pubSpeed_{0}, pubSlipPos_{-1}, pubLoopIn_{-1}, pubLoopOut_{-1}, pubSavedIn_{-1}, pubSavedOut_{-1}, pubCue_{0},
      pubTempo_{0}, pubBeat_{0};
  std::array<std::atomic<double>, kHotCues> pubHot_{};
  std::atomic<uint32_t> pubFlags_{0};
  std::atomic<uint64_t> pubWheel_{0};
  uint64_t wheelTicks_ = 0;  // J-197: the jog platter's ticks taken (a dev trace counts them)
  std::atomic<int> pubTake_{-1};
  std::atomic<float> pubLevel_{0}, pubPre_{0};
  std::atomic<uint32_t> marksSeq_{0};
};

// ================================================================== pass lanes (J-163)
// The clip instance of a lane that plays at song tick `t` (nullptr: none covers it).
const AutomationLane::Instance* coveringAt(const AutomationLane& l, double t) {
  auto it = std::upper_bound(l.inst.begin(), l.inst.end(), t, [](double v, const AutomationLane::Instance& q) { return v < static_cast<double>(q.start); });
  if (it == l.inst.begin()) return nullptr;
  --it;
  return t < static_cast<double>(it->end) && it->clip ? &*it : nullptr;
}
// A clip point's normalized value (through the clip's range, as AutomationLane::valueAt).
double clipNorm(const AutomationClip& c, double v) { return std::clamp(c.min + v * (c.max - c.min), 0.0, 1.0); }
// The last point of `c` with lo < tick <= hi whose shape is Hold (after the first point), or null.
const AutoPoint* holdIn(const AutomationClip& c, double lo, double hi) {
  const auto& P = c.points;
  auto it = std::upper_bound(P.begin(), P.end(), hi, [](double v, const AutoPoint& q) { return v < q.at(); });
  while (it != P.begin()) {
    --it;
    if (it->at() <= lo || it == P.begin()) break;
    if (it->shape == AutoShape::Hold) return &*it;
  }
  return nullptr;
}
// A pass's position lane on step `k` of its grid (origin g0, tps ticks a step): its points sit on
// the steps they hold (the first on the pass's start, the others half a step before theirs), so
// a run between two points counts whole steps: a steady run (the motor's constant travel) is
// exact. A jump (two points on one step) gives the place after it.
double passPos(const AutomationClip& c, const PosScale& sc, double g0, double tps, double k) {
  const auto& P = c.points;
  auto posOf = [&](const AutoPoint& p) { return sc.toPos(clipNorm(c, p.exact())); };
  auto stepOf = [&](size_t i) { return i == 0 ? 0.0 : std::round((P[i].at() - g0 + 0.5 * tps) / tps); };
  auto it = std::upper_bound(P.begin(), P.end(), g0 + k * tps, [](double v, const AutoPoint& q) { return v < q.at(); });
  const size_t i = it == P.begin() ? 0 : static_cast<size_t>(it - P.begin()) - 1;
  double p = posOf(P[i]);
  if (i + 1 < P.size() && P[i + 1].shape != AutoShape::Hold) {
    const double si = stepOf(i), sj = stepOf(i + 1);
    if (P[i + 1].shape != AutoShape::Curve || P[i + 1].tension != 0.0f) {
      // (a curve shaped by hand: the lane's own value where the step's point would sit)
      if (k > 0) p = sc.toPos(clipNorm(c, automationValue(c, g0 + k * tps - 0.5 * tps)));
    } else if (sj > si && k > si) {
      p += (posOf(P[i + 1]) - p) * (std::min(k, sj) - si) / (sj - si);
    }
  }
  return std::round(p * 256.0) / 256.0;
}

// ================================================================== the setup (decks + mixer)
// J-159: an input waiting for the next step start (strict mode; command keys always).
enum PendKind : uint8_t { PendIn = 0, PendCmd, PendKey, PendNone };
struct Pend {
  uint8_t kind = PendIn;
  InEvent e{};
  Cmd c{};
  uint8_t deck = 0, key = 0;
  bool down = false;
  bool held = false;  // a held-from-before press (kHeldModY)
  double at = 0, delay = 0;
  uint64_t stamp = 0;  // the sample it came on
};

class PlatterGen final : public Generator {
 public:
  PlatterGen() {
    (void)pitch::table(pitch::Quality::Realtime);
    capture_.resize(kCapture);
    decoderRaw_.reset();
    decoderHub_.reset();
    hubPos_ = hub::head();
    for (int i = 0; i < PCount; ++i) sh_.p[static_cast<size_t>(i)] = defOf(i), lastState_[static_cast<size_t>(i)] = defOf(i);
    for (int d = 0; d < kDecks; ++d) deck_[static_cast<size_t>(d)].init(d, &sh_, &deck_[static_cast<size_t>(1 - d)]);
    master0_ = master1_ = masterGainOf(defOf(PMaster));
    (void)descOf(0);  // (J-159: built here, not on the audio thread)
    lastP_ = sh_.p;
    sh_.raw = sh_.p;
  }
  ~PlatterGen() override {
    delete sh_.pool;
    delete pending_.load();
    delete retired_.load();
  }

  bool active() const override { return deck_[0].active() || deck_[1].active() || takeOn_; }
  bool ownPlayback() const override { return (deck_[0].motor_ || deck_[1].motor_) && !sh_.transport; }
  void stopOwnPlayback() override {
    for (auto& d : deck_) d.motor_ = false, d.cuePreview_ = false, d.hotPreview_ = -1, d.playLatched_ = false;
  }
  bool rawMidi() const override { return true; }
  bool wantsLanes() const override { return true; }

  // UI thread (Program build): the key lock voices for this sample rate.
  void prepareHost(const PluginState& state, double sampleRate) override {
    (void)state;
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    if (sampleRate == uiRate_) return;
    auto* p = new KeyPool;
    p->rate = sampleRate;
    for (auto& dv : p->v)
      for (auto& v : dv) v.prepare(2, sampleRate, 2.5, 2.5);
    delete pending_.exchange(p, std::memory_order_acq_rel);
    uiRate_ = sampleRate;
  }

  // ---- UI side (thread-safe)
  bool pushCmd(const Cmd& c) {
    const size_t h = cmdHead_.load(std::memory_order_relaxed);
    if (h - cmdTail_.load(std::memory_order_acquire) >= cmds_.size()) return false;
    cmds_[h % cmds_.size()] = c;
    cmdNs_[h % cmds_.size()] = hub::nowNs();
    cmdHead_.store(h + 1, std::memory_order_release);
    return true;
  }
  void view(int d, LiveView& v) const {
    deck_[static_cast<size_t>(d & 1)].view(v);
    v.rawSeq = rawSeq_.load(std::memory_order_acquire);
    v.replaying = pubReplay_.load(std::memory_order_relaxed);
    const uint32_t r = raw_.load(std::memory_order_relaxed);
    v.raw[0] = static_cast<uint8_t>(r & 0xFF), v.raw[1] = static_cast<uint8_t>((r >> 8) & 0xFF), v.raw[2] = static_cast<uint8_t>((r >> 16) & 0xFF);
  }
  void mixOut(MixView& m) const {
    m.valid = true;
    m.masterL = pubMasterL_.load(std::memory_order_relaxed);
    m.masterR = pubMasterR_.load(std::memory_order_relaxed);
    m.transport = pubTransport_.load(std::memory_order_relaxed);
    m.songBeat = pubSongBeat_.load(std::memory_order_relaxed);
    m.tempo = pubTempo_.load(std::memory_order_relaxed);
  }
  void stepOut(StepState& s) const {
    s.p = sh_.p;
    for (int k = 0; k < kDecks; ++k) {
      s.pos[k] = deck_[static_cast<size_t>(k)].pos_;
      s.vel[k] = deck_[static_cast<size_t>(k)].vel_;
      s.driven[k] = deck_[static_cast<size_t>(k)].driven_;
    }
    s.clock = sh_.clock;
  }
  void mirrorOut(Mirror& m) const {
    for (int i = 0; i < PCount; ++i) {
      m.seq[static_cast<size_t>(i)] = sh_.mirSeq[static_cast<size_t>(i)].load(std::memory_order_acquire);
      m.value[static_cast<size_t>(i)] = sh_.mirVal[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
  }
  void marksOut(int d, Marks& m) const { deck_[static_cast<size_t>(d & 1)].marksOut(m); }
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
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override {
    if (std::lround(pitch) == kStartKey) {  // J-159: a recorded performance starts (its id in mod X / Y)
      startPerformance(startIdOf(static_cast<int>(std::lround(props.modX * 256.f)), static_cast<int>(std::lround(props.modY * 256.f))));
      return;
    }
    int ci = -1;
    if (cmdKeyOf(static_cast<int>(std::lround(pitch)), ci) >= 0 && std::lround(props.modY * 256.f) == kHeldModY) {
      heldPress_ = true;  // (key() marks the press it queues)
      onNoteOn(id, pitch, velocity);
      heldPress_ = false;
      return;
    }
    onNoteOn(id, pitch, velocity);
  }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const int kind = status & 0xF0;
    noteRaw(status, d1, d2);
    if (hub::tapped()) {
      // the app reads devices from the hub: channel MIDI only plays keys (not
      // the setup's own controller buttons)
      if ((kind == 0x90 || kind == 0x80) && !boundNote(status, d1)) key(d1, kind == 0x90 && d2 > 0);
      return;
    }
    // plugin builds: the host's MIDI is the controller (on its own sample)
    InEvent ev[16];
    const int n = sh_.aux ? decoderRaw_.feed(sh_.aux->m.map, status, d1, d2, ev, 16) : 0;
    sh_.evAt = static_cast<double>(sh_.clock);
    sh_.evDelay = 0;
    for (int i = 0; i < n; ++i) input(ev[i]);
    if (n == 0 && (kind == 0x90 || kind == 0x80)) key(d1, kind == 0x90 && d2 > 0);
  }
  void onAllOff() override {
    for (auto& h : held_) h.key = -1;
    for (auto& d : deck_) d.allOff();
    if (takeOn_) stopTake();
  }
  void onKill() override {
    timedTail_ = timedHead_;
    for (auto& d : deck_) d.kill();
    if (takeOn_) stopTake();
    for (auto& h : held_) h.key = -1;
    stepPos_ = stepLen_ = 0;
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    // key lock voices from the UI thread
    if (KeyPool* np = pending_.load(std::memory_order_acquire); np && !retired_.load(std::memory_order_relaxed)) {
      pending_.store(nullptr, std::memory_order_relaxed);
      retired_.store(sh_.pool, std::memory_order_release);
      sh_.pool = np;
      for (auto& d : deck_) d.keyOn_ = false;
    }
    sh_.sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    for (auto& d : deck_) d.setEqRate(sh_.sr);
    sh_.tempo = ctx.tempo > 1 ? ctx.tempo : 120.0;
    sh_.ppq = std::max(1, ctx.ppq);
    sh_.blockTick = ctx.tick;
    sh_.blockClock = sh_.clock;
    lastBlock_ = static_cast<double>(blockAcc_);
    blockAcc_ = 0;
    // Input that arrived while the last block played (controller MIDI, editor
    // moves) is handled now, but the hand takes it at the sample time it
    // arrived (that far before this block's start), so a block's jog ticks
    // keep their spacing (offline renders: all at the block's start).
    // The block's start in wall time: a callback runs late now and then, never
    // early, so the offset between the two clocks follows the earliest ones
    // (and drifts slowly).
    const bool live = !ctx.offline;
    const double c = static_cast<double>(sh_.clock);
    if (live) {
      const double off = c - static_cast<double>(hub::nowNs()) * sh_.sr * 1e-9;
      if (!clockOffOk_ || std::fabs(off - clockOff_) > 0.1 * sh_.sr || off > clockOff_) clockOff_ = off;
      else clockOff_ += 0.002 * (off - clockOff_);
      clockOffOk_ = true;
    }
    auto arrival = [&](int64_t ns) {
      double at = c;
      if (live && ns > 0) at = std::clamp(static_cast<double>(ns) * sh_.sr * 1e-9 + clockOff_, c - 0.05 * sh_.sr, c);
      lastArrival_ = std::clamp(std::max(at, lastArrival_), c - 0.05 * sh_.sr, c);
      return lastArrival_;
    };
    sh_.transport = ctx.playing;
    recording_ = ctx.recording && ctx.playing && !ctx.offline;
    punch_ = ctx.punchOut > ctx.punchIn;
    punchIn_ = ctx.punchIn, punchOut_ = ctx.punchOut;
    // aux (model): a new one adopts its cues unless a deck set newer ones
    const auto* nx = dynamic_cast<const PlatterAux*>(aux.custom.get());
    if (nx != sh_.aux) {
      sh_.aux = nx;
      bool reset = false;
      for (auto& d : deck_) {
        const Sample* before = d.src_;
        d.adopt(nx);
        reset = reset || before != d.src_;
      }
      // a new track under a take being recorded: that take ends here (it holds its files)
      if (reset && capturing_) endCapture();
      if (reset) {
        decoderHub_.reset();
        decoderRaw_.reset();
      }
      repointTake();
    }
    // parameters: the controller's last value wins until the parameter itself changes
    for (int i = 0; i < PCount; ++i) {
      const auto k = static_cast<size_t>(i);
      const float v = par(s, i, defOf(i));
      if (v != lastState_[k]) {
        lastState_[k] = v;
        if (sh_.clock >= sh_.ctlUntil[k] || v == sh_.ctlVal[k]) sh_.ctlUntil[k] = 0;  // automation / an edit; or the editor mirrored us
      }
      sh_.raw[k] = sh_.ctlUntil[k] > 0 ? sh_.ctlVal[k] : v;
      if (!sh_.strict) sh_.p[k] = sh_.raw[k];  // (strict: at the next step start)
    }
    // a take plays its own mixer moves
    if (takeOn_)
      for (int i = 0; i < kMoveParams; ++i) {
        const auto p = static_cast<size_t>(moveParams()[static_cast<size_t>(i)]);
        if (!takeHas_[static_cast<size_t>(i)]) continue;
        sh_.raw[p] = takeVal_[static_cast<size_t>(i)];
        if (!sh_.strict) sh_.p[p] = takeVal_[static_cast<size_t>(i)];
      }
    // J-159: the tick of a sample (anchored where the position jumped or wrapped: anywhere the
    // block's tick is not where the anchor runs on to), the lanes, the loop
    const double ahead = sh_.tickAt(static_cast<double>(sh_.clock));
    auto near = [](double x, double y) { return std::fabs(x - y) <= 1e-6 * std::max(1.0, std::fabs(y)); };
    const bool onTrack = anchorOk_ && near(ahead, ctx.tick);
    // (the loop wrapped right where the last block ended: render() takes it as a wrap)
    const bool wrapsNow = anchorOk_ && !ctx.jumped && ctx.looping && ctx.loopEnd > ctx.loopStart && near(ahead, ctx.loopEnd) && near(ctx.tick, ctx.loopStart);
    if (!wrapsNow && (ctx.jumped || !onTrack || anchorTempo_ != sh_.tempo || anchorPpq_ != sh_.ppq || anchorSr_ != sh_.sr)) {
      sh_.anchorTick = ctx.tick;
      sh_.anchorClock = sh_.clock;
      anchorOk_ = true;
      anchorTempo_ = sh_.tempo, anchorPpq_ = sh_.ppq, anchorSr_ = sh_.sr;
    }
    lanes_ = ctx.playing ? ctx.lanes : nullptr;
    laneCount_ = lanes_ ? ctx.laneCount : 0;
    if (!ctx.playing || ctx.jumped) driveAt_ = -1;
    if (!ctx.playing || (ctx.jumped && lastStartAt_ < prevBlockClock_)) perfOn_ = false;
    prevBlockClock_ = sh_.clock;
    // controller MIDI from the hub (app): the setup's device
    if (hub::tapped()) {
      const uint64_t head = hub::head();
      if (head - hubPos_ > hub::kSize) hubPos_ = head - hub::kSize;
      const uint32_t want = sh_.aux ? sh_.aux->device : 0;
      InEvent ev[16];
      for (; hubPos_ < head; ++hubPos_) {
        uint32_t dev = 0;
        uint8_t st = 0, a = 0, b = 0;
        int64_t ns = 0;
        if (!hub::read(hubPos_, dev, st, a, b, &ns)) continue;
        if (want != 0 && dev != want) continue;
        noteRaw(st, a, b);
        if (!sh_.aux) continue;
        const int n = decoderHub_.feed(sh_.aux->m.map, st, a, b, ev, 16);
        sh_.evAt = arrival(ns);
        sh_.evDelay = live ? lastBlock_ : 0.0;
        for (int i = 0; i < n; ++i) {
          // J-197: the wheel (its touch, its turns) acts at the time it came, laid out over
          // this block as the ticks are: a touch, the ticks and the release stay in order
          if (live && timedInput(ev[i].control)) later(ev[i], sh_.evAt + sh_.evDelay);
          else input(ev[i]);
        }
      }
    } else {
      hubPos_ = hub::head();
    }
    // capture: begins on the next beat while Gridline records, ends when it stops or the position jumps
    const bool anyLoaded = deck_[0].loaded() || deck_[1].loaded();
    if (capturing_ && (!recording_ || ctx.jumped || !armed())) endCapture();
    if (ctx.jumped && !capturing_) capStartAt_ = -1;
    const double spt = sh_.sr * 60.0 / (sh_.tempo * sh_.ppq);  // samples per tick
    if (capturing_ && punch_ && sh_.blockTick >= punchOut_ - 1e-9) endCapture();  // (punched out)
    if (recording_ && armed() && anyLoaded && !capturing_ && capStartAt_ < 0) {
      const double beat = static_cast<double>(sh_.ppq);
      const double from = punch_ ? std::max(sh_.blockTick, punchIn_) : sh_.blockTick;  // (punch in: its beat)
      const double tb = std::ceil(from / beat - 1e-9) * beat;
      const double off = (tb - sh_.blockTick) * spt;
      if (!(ctx.looping && ctx.loopEnd > ctx.loopStart && tb >= ctx.loopEnd) && !(punch_ && tb >= punchOut_)) {  // (past the loop's end: after the wrap)
        capStartAt_ = static_cast<int64_t>(sh_.clock) + static_cast<int64_t>(std::ceil(off - 1e-9));
        capStartTick_ = tb;
      }
    }
    if (!recording_) capStartAt_ = -1;
    // J-159: punch out: the pass ends on the sample the punch range ends (as the engine finds it)
    punchEndAt_ = -1;
    if (punch_ && (capturing_ || capStartAt_ >= 0) && punchOut_ > sh_.blockTick)
      punchEndAt_ = static_cast<int64_t>(sh_.clock) + static_cast<int64_t>(std::ceil((punchOut_ - sh_.blockTick) * spt - 1e-9));
    // J-159: the sample the loop wraps on (as the engine finds it: the first one at or after the
    // loop's end, counted from where the position last landed): the tick runs on from the loop
    // start there, a performance pass ends there and the next one starts on the same sample
    wrapAt_ = -1;
    if (wrapsNow) {
      wrapAt_ = static_cast<int64_t>(sh_.clock), wrapStart_ = ctx.loopStart;
    } else if (ctx.playing && ctx.looping && ctx.loopEnd > ctx.loopStart) {
      const auto w = static_cast<int64_t>(sh_.anchorClock) + static_cast<int64_t>(std::ceil((ctx.loopEnd - sh_.anchorTick) * spt - 1e-9));
      if (w >= static_cast<int64_t>(sh_.clock)) wrapAt_ = w, wrapStart_ = ctx.loopStart;
    }
    const bool wasFollowing = following_;
    updateStrict();
    // J-163: playback starts (or jumps) while lanes play: a fresh step on this sample, so a pass
    // that starts here is picked up here
    if (following_ && (!wasFollowing || ctx.jumped)) cut();
    // editor commands
    for (size_t t = cmdTail_.load(std::memory_order_relaxed); t != cmdHead_.load(std::memory_order_acquire); ++t) {
      sh_.evAt = arrival(cmdNs_[t % cmds_.size()]);
      sh_.evDelay = live ? lastBlock_ : 0.0;
      command(cmds_[t % cmds_.size()]);
      cmdTail_.store(t + 1, std::memory_order_release);
    }
  }

  void render(float* L, float* R, int n) override {
    blockAcc_ += n;
    int i = 0;
    while (i < n) {
      if (wrapAt_ >= 0 && static_cast<int64_t>(sh_.clock) >= wrapAt_) {
        // the loop wraps: a performance pass ends here, exactly, and the next one starts here
        const bool chain = capturing_ && perfCapture_;
        if (chain) {
          cut();
          endCapture();
        }
        sh_.anchorTick = wrapStart_;
        sh_.anchorClock = sh_.clock;
        wrapAt_ = -1;
        if (following_ && !chain) cut();  // (J-163: a pass that starts on the loop start is picked up here)
        punchEndAt_ = -1;
        const bool inPunch = !punch_ || (wrapStart_ >= punchIn_ - 1e-9 && wrapStart_ < punchOut_);
        if (chain && inPunch && recording_ && armed() && (deck_[0].loaded() || deck_[1].loaded())) {
          capStartAt_ = -1;
          beginCapture();
          if (punch_)  // (where it punches out after the wrap)
            punchEndAt_ = static_cast<int64_t>(sh_.clock) +
                          static_cast<int64_t>(std::ceil((punchOut_ - wrapStart_) * sh_.sr * 60.0 / (sh_.tempo * sh_.ppq) - 1e-9));
        }
        updateStrict();
      }
      if (punchEndAt_ >= 0 && static_cast<int64_t>(sh_.clock) >= punchEndAt_) {
        if (capturing_) {
          cut();  // punched out: the pass ends exactly here
          endCapture();
          updateStrict();
        }
        capStartAt_ = -1;
        punchEndAt_ = -1;
      }
      if (capStartAt_ >= 0 && !capturing_) {
        if (static_cast<int64_t>(sh_.clock) >= capStartAt_) {
          cut();  // the take starts exactly here, on a fresh step
          beginCapture();
        }
      }
      if (driveAt_ >= 0 && static_cast<int64_t>(sh_.clock) >= driveAt_) {
        cut();  // J-163: a pass is picked up here, on its own step grid
        driveAt_ = -1;
      }
      if (stepPos_ >= stepLen_) computeStep();
      int run = std::min(n - i, stepLen_ - stepPos_);
      if (capStartAt_ >= 0 && !capturing_ && static_cast<int64_t>(sh_.clock) < capStartAt_)
        run = static_cast<int>(std::min<int64_t>(run, capStartAt_ - static_cast<int64_t>(sh_.clock)));
      if (wrapAt_ >= 0 && static_cast<int64_t>(sh_.clock) < wrapAt_)
        run = static_cast<int>(std::min<int64_t>(run, wrapAt_ - static_cast<int64_t>(sh_.clock)));
      if (punchEndAt_ >= 0 && static_cast<int64_t>(sh_.clock) < punchEndAt_)
        run = static_cast<int>(std::min<int64_t>(run, punchEndAt_ - static_cast<int64_t>(sh_.clock)));
      if (driveAt_ >= 0 && static_cast<int64_t>(sh_.clock) < driveAt_)
        run = static_cast<int>(std::min<int64_t>(run, driveAt_ - static_cast<int64_t>(sh_.clock)));
      runStep(L + i, R + i, run);
      i += run;
    }
    publish();
  }
  bool handlesNotePan() const override { return true; }

 private:
  // ---------------------------------------------------------------- helpers
  bool armed() const { return sh_.aux && sh_.aux->m.armed; }
  bool boundNote(uint8_t s, uint8_t d1) const {
    if (!sh_.aux) return false;
    const int ch = s & 0x0F;
    for (const Binding& b : sh_.aux->m.map.bindings)
      if (b.kind == MkNote && b.num == d1 && (b.channel < 0 || b.channel == ch)) return true;
    return false;
  }
  void noteRaw(uint8_t s, uint8_t a, uint8_t b) {
    raw_.store(static_cast<uint32_t>(s) | (static_cast<uint32_t>(a) << 8) | (static_cast<uint32_t>(b) << 16), std::memory_order_relaxed);
    rawSeq_.fetch_add(1, std::memory_order_release);
  }

  // ---------------------------------------------------------------- inputs
  // J-197: the controller's wheel waits for the sample it arrived on (live input is laid out
  // over the next block); everything else acts at once.
  static bool timedInput(int c) { return c == CtlTouch || c == CtlJogPlatter || c == CtlJogRim || c == CtlJogSearch; }
  void later(const InEvent& e, double at) {
    if (timedHead_ - timedTail_ >= timed_.size()) runTimed(std::numeric_limits<double>::infinity(), 1);  // (full: the oldest now)
    timed_[timedHead_ % timed_.size()] = Timed{at, e};
    ++timedHead_;
  }
  // The waiting wheel input up to sample time `upTo` (at most `max` of them), in order. (A
  // stamp a hair past it, the clocks' rounding, counts as on it.)
  void runTimed(double upTo, size_t max = ~size_t{0}) {
    for (size_t k = 0; k < max && timedTail_ != timedHead_; ++k) {
      const Timed& t = timed_[timedTail_ % timed_.size()];
      if (t.at > upTo + 1e-3) break;
      sh_.evAt = std::min(t.at, upTo);
      sh_.evDelay = 0;
      const InEvent e = t.e;
      ++timedTail_;
      input(e);
    }
  }
  void input(const InEvent& e) {
    if (e.control == CtlJogPlatter) deck_[static_cast<size_t>(e.deck & 1)].countWheel(e.ticks);
    if (takeOn_ && e.control != CtlShift && e.control != CtlNone && controlInfo(e.control).type != TypeAbsolute) stopTake();  // the hand takes over
    switch (e.control) {
      case CtlXfader: sh_.setCtl(PXfader, e.value); break;
      case CtlMaster: {  // J-160: 0 dB at the knob's centre, like the editor's dial
        const double v = std::clamp(static_cast<double>(e.value), 0.0, 1.0);
        sh_.setCtl(PMaster, static_cast<float>(v <= 0.5 ? kMasterOffDb * (1.0 - 2.0 * v) : (2.0 * v - 1.0) * 6.0));
        break;
      }
      default:
        if (sh_.strict && stepInput(e.control)) {  // J-159: acts on the next step start
          Pend q;
          q.kind = PendIn;
          q.e = e;
          push(q);
          break;
        }
        deck_[static_cast<size_t>(e.deck & 1)].input(e);
        break;
    }
  }
  void command(const Cmd& c) {
    if (takeOn_ && (c.kind == CmdHandDown || c.kind == CmdSeek || (c.kind == CmdButton && c.a != CtlShift))) stopTake();
    if (c.kind == CmdAbsolute && (c.a == CtlXfader || c.a == CtlMaster)) {
      InEvent e;
      e.control = c.a;
      e.type = TypeAbsolute;
      e.value = static_cast<float>(c.v);
      input(e);
      return;
    }
    if (deck_[static_cast<size_t>(c.deck & 1)].driven_ && c.kind != CmdAbsolute) return;  // J-163: a pass plays it
    if (sh_.strict) {  // J-159
      if (c.kind == CmdButton) {
        InEvent e;
        e.control = c.a;
        e.down = c.v != 0;
        e.deck = static_cast<uint8_t>(c.deck & 1);
        input(e);
        return;
      }
      if (c.kind == CmdHandDown || c.kind == CmdHandUp || c.kind == CmdHandMove || c.kind == CmdSeek) {
        Pend q;
        q.kind = PendCmd;
        q.c = c;
        push(q);
        return;
      }
    }
    deck_[static_cast<size_t>(c.deck & 1)].command(c);
  }
  // Inputs that wait for the step start in strict mode: buttons, jog and search ticks (the
  // parameters, the mode toggles and shift act at once: the step start reads them anyway).
  static bool stepInput(int c) {
    if (c == CtlNone || c == CtlShift || c == CtlSync || c == CtlKeyLock || c == CtlSlip || c == CtlVinyl || c == CtlQuantize || c == CtlLoaded)
      return false;
    return controlInfo(c).type != TypeAbsolute;
  }
  void push(Pend q) {
    if (pendHead_ - pendTail_ >= pend_.size()) return;
    q.at = sh_.evAt;
    q.delay = sh_.evDelay;
    q.stamp = sh_.clock;
    pend_[pendHead_ % pend_.size()] = q;
    ++pendHead_;
  }

  // Keys: 36..59 takes, 60..67 deck A hot cues, 72..79 deck B hot cues.
  void key(int k, bool down) {
    // J-159: a command key holds a deck button (from the next step start)
    int ci = -1;
    if (const int cd = cmdKeyOf(k, ci); cd >= 0) {
      if (capturing_) return;  // (the hands play while a pass is recorded)
      if (down && heldPress_ && lastStartAt_ == sh_.clock && perfOn_) return;  // the start state has it down
      Pend q;
      q.kind = PendKey;
      q.deck = static_cast<uint8_t>(cd);
      q.key = static_cast<uint8_t>(ci);
      q.down = down;
      q.held = down && heldPress_;
      sh_.evAt = static_cast<double>(sh_.clock);
      sh_.evDelay = 0;
      push(q);
      return;
    }
    if (k == kStartKey) return;
    // a note (render() is split at every event) cuts the running step here, so
    // what it starts lands on its exact sample, whatever the block size (not while a
    // pass is captured, nor in strict mode: the steps run on from the pass's start)
    if (!capturing_ && !sh_.strict) cut();
    for (int d = 0; d < kDecks; ++d) {
      const int first = d == 0 ? kHotKeyFirst : kHotKeyFirstB;
      if (k >= first && k < first + kHotCues) {
        if (deck_[static_cast<size_t>(d)].driven_) return;  // (J-163: a pass plays the deck)
        if (down && takeOn_) stopTake();
        deck_[static_cast<size_t>(d)].hotKey(k - first, down);
        return;
      }
    }
    if (k >= kTakeKeyFirst && k < kTakeKeyFirst + kTakeKeys && sh_.aux) {
      if (down) {
        const auto& takes = sh_.aux->m.takes;
        for (size_t t = 0; t < takes.size(); ++t)
          if (takes[t].key == k && takes[t].steps() > 0) {
            startTake(static_cast<int>(t));
            break;
          }
      } else if (takeOn_ && takeKey_ == k) {
        stopTake();
      }
    }
  }

  // ---------------------------------------------------------------- takes
  const Take& take() const { return sh_.aux->m.takes[static_cast<size_t>(takeIndex_)]; }
  void startTake(int index) {
    const Take& t = sh_.aux->m.takes[static_cast<size_t>(index)];
    takeOn_ = true;
    takeIndex_ = index;
    takeKey_ = t.key;
    takeT_ = 0;
    takeSteps_ = t.steps();
    takeStepOut_ = static_cast<double>(t.step) * (sh_.sr / t.sampleRate) * (t.tempo / sh_.tempo);
    moveAt_ = 0;
    takeHas_.fill(false);
    for (int d = 0; d < kDecks; ++d) {
      const TakeStream& s = t.s[static_cast<size_t>(d)];
      deck_[static_cast<size_t>(d)].startTake(&s, s.on ? sh_.aux->file(s.file) : nullptr);
    }
  }
  void stopTake(bool ranOut = false) {
    if (!takeOn_) return;
    takeOn_ = false;
    takeKey_ = -1;
    for (auto& d : deck_) d.endTake(ranOut);
  }
  // The aux changed under a playing take: find it again (by key) or stop.
  void repointTake() {
    if (!takeOn_) return;
    if (!sh_.aux) {
      stopTake();
      return;
    }
    const auto& takes = sh_.aux->m.takes;
    int found = -1;
    for (size_t t = 0; t < takes.size(); ++t)
      if (takes[t].key == takeKey_ && takes[t].steps() == takeSteps_) found = static_cast<int>(t);
    if (found < 0) {
      stopTake();
      return;
    }
    takeIndex_ = found;
    for (int d = 0; d < kDecks; ++d) {
      const TakeStream& s = takes[static_cast<size_t>(found)].s[static_cast<size_t>(d)];
      deck_[static_cast<size_t>(d)].repointTake(&s, s.on ? sh_.aux->file(s.file) : nullptr);
    }
  }
  // Mixer moves of the take up to step `upTo` (inclusive).
  void applyMoves(double upTo) {
    const auto& mv = take().moves;
    const auto& mp = moveParams();
    while (moveAt_ < mv.size() && static_cast<double>(mv[moveAt_].step) <= upTo + 1e-9) {
      const TakeMove& m = mv[moveAt_++];
      for (int i = 0; i < kMoveParams; ++i)
        if (mp[static_cast<size_t>(i)] == m.param) {
          takeHas_[static_cast<size_t>(i)] = true;
          takeVal_[static_cast<size_t>(i)] = m.value;
          sh_.p[m.param] = m.value;
          sh_.raw[m.param] = m.value;
        }
    }
  }

  void beginCapture() {
    capturing_ = true;
    capStartAt_ = -1;
    // J-159: a performance (pattern data) or a take, as the setup says
    perfCapture_ = sh_.aux && sh_.aux->m.recordAs == RecAutomation;
    CaptureEvent e;
    e.kind = CaptureEvent::Start;
    e.tick = capStartTick_;
    e.tempo = sh_.tempo;
    e.sampleRate = sh_.sr;
    e.ppq = sh_.ppq;
    for (int d = 0; d < kDecks; ++d) {
      Deck& k = deck_[static_cast<size_t>(d)];
      if (!k.loaded()) continue;
      e.streams = static_cast<uint8_t>(e.streams | (1u << d));
      e.file[d] = k.fileIdx_;
      e.startRate[d] = k.vel_;
      e.pt[d].pos = k.jumpPending_ ? k.jumpTo_ : k.pos_;
      e.pt[d].from = k.pos_;  // (J-163: the read head itself; a jump waiting is the first step's)
      e.gain[d] = k.lastGain_;
      e.frames[d] = k.scale().frames;
      e.srcRate[d] = k.src_ ? k.src_->sampleRate : sh_.sr;
      k.fresh_ = true;  // the take's first step is straight (its replay starts so)
      if (k.keyOn_) k.keyRestart_ = true;  // the key lock engine starts fresh with the take (replays match)
    }
    size_t snapN = 0;
    if (perfCapture_) {
      // from here the decks work in whole steps; the state they start from is kept (the
      // start note of the performance restores it)
      if (!sh_.strict) lastP_ = sh_.p;
      for (int i = 0; i < PCount; ++i)  // (the controls as the pass's first step uses them)
        startN_[static_cast<size_t>(i)] = perfParam(i) ? 0.f : normOf(i, sh_.raw[static_cast<size_t>(i)]);
      startHeld_ = false;
      following_ = false;
      sh_.replay = false;
      sh_.strict = true;
      for (auto& d : deck_) {
        d.wheelLane_ = false;
        d.perfReset();
      }
      snapAll([&](double& x) {
        if (snapN < snapBuf_.size()) snapBuf_[snapN] = x;
        ++snapN;
      });
      e.snap[0] = static_cast<double>(snapN);
      e.snap[1] = laneTick();
      capN_.fill(std::numeric_limits<float>::quiet_NaN());
      capS_.fill(std::numeric_limits<double>::quiet_NaN());
      for (auto& d : deck_) d.motorSnap_ = false;
    }
    pushCapture(e);
    capSteps_ = 0;
    capSamples_ = 0;
    if (perfCapture_) {
      for (size_t c = 0; c * kSnapPer < snapN; ++c) {
        CaptureEvent m;
        m.kind = CaptureEvent::Snap;
        m.step = static_cast<uint32_t>(c);
        for (size_t j = 0; j < static_cast<size_t>(kSnapPer) && c * kSnapPer + j < snapN; ++j) m.snap[j] = snapBuf_[c * kSnapPer + j];
        pushCapture(m);
      }
      return;  // (the values come with the first step)
    }
    // the mixer's values at the start (step 0), then every change
    const auto& mp = moveParams();
    for (int i = 0; i < kMoveParams; ++i) {
      capVal_[static_cast<size_t>(i)] = sh_.p[mp[static_cast<size_t>(i)]];
      CaptureEvent m;
      m.kind = CaptureEvent::Move;
      m.step = 0;
      m.param = mp[static_cast<size_t>(i)];
      m.value = capVal_[static_cast<size_t>(i)];
      pushCapture(m);
    }
  }
  void endCapture() {
    if (!capturing_) return;
    capturing_ = false;
    CaptureEvent e;
    e.kind = CaptureEvent::End;
    e.tick = perfCapture_ ? laneTick() : capStartTick_ + static_cast<double>(capSamples_) * sh_.tempo * sh_.ppq / (60.0 * sh_.sr);
    e.step = static_cast<uint32_t>(capSteps_);
    pushCapture(e);
  }
  void pushCapture(const CaptureEvent& e) {
    const size_t h = capHead_.load(std::memory_order_relaxed);
    if (h - capTail_.load(std::memory_order_acquire) >= capture_.size() - 1) {
      capLost_ = true;
      return;
    }
    const bool bulk = e.kind == CaptureEvent::Point || e.kind == CaptureEvent::Move || e.kind == CaptureEvent::Button || e.kind == CaptureEvent::Snap;
    if (capLost_ && bulk) return;
    CaptureEvent& slot = capture_[h % capture_.size()];
    slot = e;
    if (capLost_ && e.kind == CaptureEvent::End) slot.kind = CaptureEvent::Lost;  // the editor drops a take with holes
    capHead_.store(h + 1, std::memory_order_release);
    if (!bulk) capLost_ = false;
  }

  // ---------------------------------------------------------------- the step
  void computeStep() {
    const int len = kStep;
    stepLen_ = len;
    stepPos_ = 0;
    // J-197: the wheel input that arrived for this step (its ticks keep their own times)
    if (timedTail_ != timedHead_) {
      runTimed(static_cast<double>(sh_.clock));
      sh_.evAt = static_cast<double>(sh_.clock), sh_.evDelay = 0;
    }
    // a take: its mixer moves land on their steps
    double adv = 0;
    bool exact = false;
    if (takeOn_) {
      adv = static_cast<double>(len) / takeStepOut_;
      exact = std::fabs(takeStepOut_ - kStep) < 1e-9 && std::fabs(takeT_ - std::round(takeT_)) < 1e-9;
      applyMoves(std::floor(takeT_ + 1e-9));
    }
    // J-159: strict: the values of this step (lanes or what came in), then the inputs
    // waiting for it (command keys always wait for a step start)
    if (sh_.strict) {
      laneScan();
      driveScan();
      stepValues();
    }
    runPending();
    if (capturing_ && perfCapture_) captureState();  // J-163: the decks' state as the step uses it
    for (auto& d : deck_) d.motorSnap_ = false;
    // capture (take): mixer values that changed since the last step
    if (capturing_ && !perfCapture_) {
      const auto& mp = moveParams();
      for (int i = 0; i < kMoveParams; ++i) {
        const float v = sh_.p[mp[static_cast<size_t>(i)]];
        if (v == capVal_[static_cast<size_t>(i)]) continue;
        capVal_[static_cast<size_t>(i)] = v;
        CaptureEvent m;
        m.kind = CaptureEvent::Move;
        m.step = static_cast<uint32_t>(capSteps_);
        m.param = mp[static_cast<size_t>(i)];
        m.value = v;
        pushCapture(m);
      }
    }
    // sync: who follows whom (the project, or the other deck)
    for (auto& d : deck_) d.preStep();
    const bool toDeck = static_cast<int>(std::lround(sh_.p[PSyncTo])) == SyncDeck;
    for (int i = 0; i < kDecks; ++i) {
      Deck& d = deck_[static_cast<size_t>(i)];
      Deck& o = deck_[static_cast<size_t>(1 - i)];
      if (!d.on(DSync)) {
        d.leader_ = Deck::kLeadNone;
        continue;
      }
      // both synced: B follows A, A follows the project
      const bool followDeck = toDeck && o.loaded() && !o.inTake_ && (!o.on(DSync) || i == 1);
      d.leader_ = followDeck ? Deck::kLeadDeck : Deck::kLeadProject;
      d.syncBpm_ = followDeck && !o.on(DSync) ? o.ownBpm() : sh_.tempo;
    }
    for (auto& d : deck_) d.computeStep(len, takeT_, adv, takeStepOut_, exact);
    // master level ramps over the step
    master0_ = master1_;
    master1_ = masterGainOf(sh_.p[PMaster]);
    if (takeOn_) {
      takeT_ += adv;
      if (takeT_ >= static_cast<double>(takeSteps_) - 1e-9) stopTake(true);  // the take is over
    }
    if (capturing_) {
      if (perfCapture_) {
        // the performance lanes' values after this step (a seek, the mouse, the wheel, nudges)
        for (int d = 0; d < kDecks; ++d)
          for (int dp : {static_cast<int>(DPos), static_cast<int>(DMouse), static_cast<int>(DWheel), static_cast<int>(DNudge)}) {
            const int p = deckParam(d, dp);
            const float n = deck_[static_cast<size_t>(d)].laneOut(dp);
            if (n == capN_[static_cast<size_t>(p)]) continue;
            capN_[static_cast<size_t>(p)] = n;
            capMove(p, n);
          }
      }
      // the read heads' path (a take's points; J-163: a pass's position lanes)
      CaptureEvent e;
      e.kind = CaptureEvent::Point;
      e.step = static_cast<uint32_t>(capSteps_);
      for (int d = 0; d < kDecks; ++d) e.pt[d] = deck_[static_cast<size_t>(d)].pt_;
      pushCapture(e);
      ++capSteps_;
      capSamples_ += static_cast<uint64_t>(len);
    }
    lastP_ = sh_.p;
  }

  // ---------------------------------------------------------------- performances (J-159)
  void capMove(int p, double n, uint8_t flag = 0) {
    CaptureEvent m;
    m.kind = CaptureEvent::Move;
    m.step = static_cast<uint32_t>(capSteps_);
    m.param = static_cast<uint8_t>(p);
    m.value = n;
    m.flag = flag;
    pushCapture(m);
  }
  // J-163: each deck's state after the step's inputs (the state lanes of the pass): what changed,
  // and the play lane wherever the motor started or stopped at once.
  void captureState() {
    for (int d = 0; d < kDecks; ++d) {
      const Deck& k = deck_[static_cast<size_t>(d)];
      if (!k.loaded()) continue;
      const PosScale sc = k.scale();
      for (int dp = DPlay; dp < DAll; ++dp) {
        const int p = deckParam(d, dp);
        double v = 0;
        switch (dp) {
          case DPlay: v = k.motor_ ? 1 : 0; break;
          case DTouch: v = k.touchOn() ? 1 : 0; break;
          case DHand: v = k.mouseOn() ? 1 : 0; break;
          case DLoop: v = k.loopOn_ ? 1 : 0; break;
          case DLoopIn: v = sc.toN(k.loopIn_); break;
          case DLoopOut: v = sc.toN(k.loopOut_); break;
          case DReverse: v = k.censor_ ? 1 : 0; break;
          default: v = normOf(p, k.bendState()); break;
        }
        const bool snap = dp == DPlay && k.motorSnap_;
        if (v == capS_[static_cast<size_t>(p)] && !snap) continue;
        capS_[static_cast<size_t>(p)] = v;
        capMove(p, v, snap ? 1 : 0);
      }
    }
  }
  double ticksPerStep() const { return static_cast<double>(kStep) * sh_.tempo * sh_.ppq / (60.0 * sh_.sr); }
  // Step start (following): the lanes that play now. The performance lanes (a pass's own:
  // travel, nudges, state) count only where a clip or pattern holds them; the controls' lanes
  // hold their values around their clips as automation does.
  void laneScan() {
    sh_.laneHas.fill(false);
    laneClip_.fill(nullptr);
    if (!following_ || !lanes_) return;
    const double t = laneTick();
    // J-179: a parameter can have two lanes (a pass's pattern clips, an older pass's playlist
    // clips): they play as one lane would. The clip playing now wins (the later started one
    // where both play), else the one that ended last (it holds its value).
    std::array<bool, PCount> cov{};
    std::array<double, PCount> key{};
    for (int i = 0; i < laneCount_; ++i) {
      const ParamLane& l = lanes_[i];
      if (l.param < 0 || l.param >= PCount || !l.lane) continue;
      const auto k = static_cast<size_t>(l.param);
      const double lt = t - l.origin;
      const AutomationLane::Instance* in = coveringAt(*l.lane, lt);
      double rank = -1e300;  // (not covered: the end of the instance before it, if any)
      if (in) {
        rank = static_cast<double>(in->start);
      } else {
        auto it = std::upper_bound(l.lane->inst.begin(), l.lane->inst.end(), lt,
                                   [](double v, const AutomationLane::Instance& q) { return v < static_cast<double>(q.start); });
        if (it != l.lane->inst.begin()) rank = static_cast<double>((it - 1)->end);
      }
      if (sh_.laneHas[k] && (cov[k] ? !in || rank < key[k] : in == nullptr && rank < key[k])) continue;
      if (!perfParam(l.param)) {
        sh_.laneHas[k] = true;
        sh_.laneN[k] = l.lane->valueAt(lt);
        cov[k] = in != nullptr;
        key[k] = rank;
        continue;
      }
      if (!in) continue;
      cov[k] = true;
      key[k] = rank;
      const AutomationClip& c = *in->clip;
      if (c.points.empty()) continue;
      double ct = static_cast<double>(in->offset) + ((t - l.origin) - static_cast<double>(in->start));
      sh_.laneHas[k] = true;
      sh_.laneN[k] = clipNorm(c, automationValue(c, ct));
      if (c.loop > 0 && ct >= static_cast<double>(c.loop)) ct = std::fmod(ct, static_cast<double>(c.loop));
      laneClip_[k] = &c;
      laneC_[k] = ct;
    }
  }
  // Step start (following): which decks a pass's state lanes drive. A deck picks a pass up (a
  // new one, or the place jumped: a cut, a moved clip, playback started in it) on the pass's own
  // step grid: the render splits there, at most a step from now. On the pass's first step its
  // start state comes back (exact); anywhere else the deck takes the lanes' state (chase).
  void driveScan() {
    chaseNow_.fill(false);
    const double tps = ticksPerStep(), spt = sh_.sr * 60.0 / (sh_.tempo * sh_.ppq);
    int want[kDecks] = {0, 0};
    for (int d = 0; d < kDecks; ++d) {
      Deck& k = deck_[static_cast<size_t>(d)];
      PassRef& r = pass_[static_cast<size_t>(d)];
      int key = -1;
      for (int dp : {static_cast<int>(DTrack), static_cast<int>(DPlay), static_cast<int>(DTouch), static_cast<int>(DHand), static_cast<int>(DLoop),
                     static_cast<int>(DReverse), static_cast<int>(DBend), static_cast<int>(DLoopIn), static_cast<int>(DLoopOut)}) {
        const auto p = static_cast<size_t>(deckParam(d, dp));
        if (sh_.laneHas[p] && laneClip_[p]) {
          key = static_cast<int>(p);
          break;
        }
      }
      if (key < 0 || !k.loaded()) {
        r.on = false;
        k.driven_ = false;
        continue;
      }
      const AutomationClip& c = *laneClip_[static_cast<size_t>(key)];
      const AutoPoint& f = c.points.front();
      const double ct = laneC_[static_cast<size_t>(key)];
      if (r.on && r.key == key && r.g0 == f.at() && r.first == f.exact() && r.tag == f.tension && std::fabs(ct - (r.lastC + tps)) < 0.25 * tps) {
        r.lastC = ct;
        continue;
      }
      // (the pass's grid may fall between two samples here: a step less than a sample ago counts
      // as now, else the sample after it would never be on it)
      const double kk = std::max(0.0, std::ceil((ct - f.at()) / tps - 1.0 / kStep - 1e-6));
      const double off = (f.at() + kk * tps - ct) * spt;
      const double n = off <= 1e-3 ? 0.0 : std::ceil(off - 1e-3);
      if (n > 0) {  // (free until the pass.s next step)
        r.on = false;
        k.driven_ = false;
        const int64_t at = static_cast<int64_t>(sh_.clock) + static_cast<int64_t>(n);
        if (driveAt_ < 0 || at < driveAt_) driveAt_ = at;
        continue;
      }
      r.on = true, r.key = key, r.g0 = f.at(), r.first = f.exact(), r.tag = f.tension, r.lastC = ct;
      k.driven_ = true;
      chaseNow_[static_cast<size_t>(d)] = true;
      primed_[static_cast<size_t>(d)] = false;
      if (key == deckParam(d, DTrack)) {
        const int id = startIdOfTag(f.tension);
        if (kk == 0) want[d] = id;
        else holdStart(id);  // (the controls the pass did not move keep their values from it)
      }
    }
    // the pass's first step: its start state (both decks it drives at once)
    for (int d = 0; d < kDecks; ++d) {
      if (want[d] <= 0) continue;
      unsigned mask = 0;
      for (int e = d; e < kDecks; ++e)
        if (want[e] == want[d]) mask |= 1u << e;
      const int id = want[d];
      if (restoreStart(id, mask))
        for (int e = 0; e < kDecks; ++e)
          if ((mask >> e) & 1u) chaseNow_[static_cast<size_t>(e)] = false, primed_[static_cast<size_t>(e)] = true;
      for (int e = d; e < kDecks; ++e)
        if (want[e] == id) want[e] = 0;
    }
  }
  // A pass's start state for the decks in `mask` (the mixer's part as well); false when the
  // setup does not have it (or it is from another layout).
  bool restoreStart(int id, unsigned mask) {
    if (!sh_.aux) return false;
    const PerfStart* ps = nullptr;
    for (const auto& st : sh_.aux->m.starts)
      if (st.id == id) ps = &st;
    if (!ps || ps->v.size() != snapCount()) return false;
    struct MarksKeep {
      double cue;
      std::array<double, kHotCues> hot;
      double in, out;
    } keep[kDecks];
    for (int d = 0; d < kDecks; ++d) {
      const Deck& k = deck_[static_cast<size_t>(d)];
      keep[d] = MarksKeep{k.cue_, k.hot_, k.savedIn_, k.savedOut_};
    }
    const size_t head = 2 + 2 * static_cast<size_t>(kLegacyPCount), per = (ps->v.size() - head) / kDecks;
    size_t i = 0;
    snapAll([&](double& x) {
      const size_t j = i++;
      if (j < head || ((mask >> ((j - head) / per)) & 1u)) x = ps->v[j];
    });
    for (int d = 0; d < kDecks; ++d) {
      Deck& k = deck_[static_cast<size_t>(d)];
      k.cue_ = keep[d].cue, k.hot_ = keep[d].hot, k.savedIn_ = keep[d].in, k.savedOut_ = keep[d].out;
      if ((mask >> d) & 1u) k.passRestored();
    }
    // the controls without a lane keep the values they had in the pass until they change here
    startRaw_ = sh_.raw;
    startHeld_ = true;
    return true;
  }
  // A pass picked up in the middle: the controls it did not move keep the values they had in
  // it (its start state holds them) until they change here.
  void holdStart(int id) {
    if (!sh_.aux || id <= 0) return;
    for (const auto& st : sh_.aux->m.starts) {
      if (st.id != id || st.v.size() != snapCount()) continue;
      const size_t at = 2 + static_cast<size_t>(kLegacyPCount);  // (master ramp, the last step's values, then these)
      for (size_t i = 0; i < static_cast<size_t>(kLegacyPCount); ++i) startN_[i] = static_cast<float>(st.v[at + i]);
      startRaw_ = sh_.raw;
      startHeld_ = true;
      return;
    }
  }
  // What deck `d`'s state lanes say for this step (it is driven).
  DriveStep driveStep(int d) {
    DriveStep s;
    s.chase = chaseNow_[static_cast<size_t>(d)];
    auto st = [&](int dp) {
      const auto p = static_cast<size_t>(deckParam(d, dp));
      return sh_.laneHas[p] ? static_cast<int>(std::lround(valueOfN(static_cast<int>(p), sh_.laneN[p]))) : -1;
    };
    s.play = st(DPlay), s.touch = st(DTouch), s.hand = st(DHand), s.loop = st(DLoop), s.rev = st(DReverse), s.bend = st(DBend);
    const Deck& k = deck_[static_cast<size_t>(d)];
    const PosScale sc = k.scale();
    const double tps = ticksPerStep();
    const auto pPlay = static_cast<size_t>(deckParam(d, DPlay)), pIn = static_cast<size_t>(deckParam(d, DLoopIn)),
               pOut = static_cast<size_t>(deckParam(d, DLoopOut)), pTrack = static_cast<size_t>(deckParam(d, DTrack));
    if (laneClip_[pPlay]) {
      const AutoPoint* h = holdIn(*laneClip_[pPlay], laneC_[pPlay] - 0.75 * tps, laneC_[pPlay] + 0.25 * tps);
      s.instant = h && h->tension == kInstantTension;
    }
    if (sh_.laneHas[pIn]) s.hasIn = true, s.loopIn = sc.toPos(sh_.laneN[pIn]);
    if (sh_.laneHas[pOut]) s.hasOut = true, s.loopOut = sc.toPos(sh_.laneN[pOut]);
    if (const AutomationClip* c = laneClip_[pTrack]) {
      const double ct = laneC_[pTrack];
      const double g0 = c->points.front().at();
      const double step = std::max(0.0, std::round((ct - g0) / tps));
      if (s.chase) {
        s.hasPos = true;
        s.pos = passPos(*c, sc, g0, tps, step);
        s.vel = (passPos(*c, sc, g0, tps, step + 1) - s.pos) / kStep;
        const auto pWheel = static_cast<size_t>(deckParam(d, DWheel)), pTouch = static_cast<size_t>(deckParam(d, DTouch));
        const AutomationClip *cw = laneClip_[pWheel], *cto = laneClip_[pTouch];
        if (s.touch > 0 && cw && cto) {
          // the steps the hand was on the wheel (up to kWarm back, within the clips)
          const double wt = laneC_[pWheel], tt = laneC_[pTouch];
          int n = 0;
          while (n < DriveStep::kWarm && step - n - 1 >= 0) {
            const double back = (n + 1) * tps;
            if (wt - back < cw->points.front().at() || tt - back < cto->points.front().at()) break;
            if (valueOfN(static_cast<int>(pTouch), clipNorm(*cto, automationValue(*cto, tt - back))) < 0.5f) break;
            ++n;
          }
          const auto turns = [&](int j) {  // (the wheel lane j steps back)
            return static_cast<double>(valueOfN(static_cast<int>(pWheel), static_cast<float>(clipNorm(*cw, automationValue(*cw, wt - j * tps)))));
          };
          s.warmN = n;
          for (int i = 0; i < n; ++i) warmDx_[static_cast<size_t>(i)] = Deck::wrapTurns(turns(n - i) - turns(n - i + 1)) * kSecondsPerRev;
          s.warmDx = warmDx_.data();
          const double s0 = step - n;
          s.warmV = (passPos(*c, sc, g0, tps, s0) - passPos(*c, sc, g0, tps, s0 - 1)) / kStep;
        }
      } else if (const AutoPoint* h = holdIn(*c, ct - 0.75 * tps, ct + 0.25 * tps)) {
        s.jump = true;
        s.jumpTo = std::round(sc.toPos(clipNorm(*c, h->exact())) * 256.0) / 256.0;
      } else {
        s.check = true;
        s.lanePos = passPos(*c, sc, g0, tps, step);
      }
    }
    return s;
  }
  // The transport tick of this sample (the engine splits its blocks where the loop wraps).
  double laneTick() const { return sh_.tickAt(static_cast<double>(sh_.clock)); }
  // Strict mode on (a performance captured, lanes followed) or off; off applies what waits.
  void updateStrict() {
    const bool wasFollowing = following_;
    following_ = sh_.transport && !capturing_ && (laneCount_ > 0 || perfOn_);
    if (following_ && !wasFollowing && !primedByStart_) primed_.fill(false);
    if (!following_) {  // (J-163: no pass drives a deck now)
      for (auto& r : pass_) r.on = false;
      for (auto& d : deck_) d.driven_ = false;
      driveAt_ = -1;
    }
    primedByStart_ = false;
    sh_.replay = following_;
    const bool strict = (capturing_ && perfCapture_) || following_;
    if (strict == sh_.strict) return;
    if (strict) {
      sh_.strict = true;
      return;
    }
    sh_.strict = false;
    sh_.p = sh_.raw;
    for (auto& d : deck_) d.wheelLane_ = false;
    for (int guard = 0; pendTail_ != pendHead_ && guard < 64; ++guard) runPending();
  }
  // Step start (strict): every parameter's value for this step: its lane while lanes play,
  // else what came in, as a lane would give it back. A pass records the changes.
  void stepValues() {
    const bool rec = capturing_ && perfCapture_;
    for (int i = 0; i < PCount; ++i) {
      if (perfParam(i)) continue;
      const auto k = static_cast<size_t>(i);
      const bool held = startHeld_ && (perfOn_ || deck_[0].driven_ || deck_[1].driven_) && sh_.raw[k] == startRaw_[k];
      const double n = sh_.laneHas[k] ? sh_.laneN[k] : held ? static_cast<double>(startN_[k]) : static_cast<double>(normOf(i, sh_.raw[k]));
      sh_.p[k] = valueOfN(i, n);
      if (rec) {
        const auto nf = static_cast<float>(n);
        if (!(nf == capN_[k])) {
          capN_[k] = nf;
          capMove(i, nf);
        }
      }
    }
    // a deck synced while it plays falls in phase now (what the Sync button did at once)
    for (int d = 0; d < kDecks; ++d) {
      const auto sp = static_cast<size_t>(deckParam(d, DSync));
      Deck& k = deck_[static_cast<size_t>(d)];
      if (lastP_[sp] < 0.5f && sh_.p[sp] >= 0.5f && k.motor_) {
        k.motor_ = false;
        k.startMotor(false);
      }
    }
  }
  // Is a queued input a button (which deck, which key)?
  static bool pendKey(const Pend& q, int& d, int& key) {
    if (q.kind == PendNone) return false;
    if (q.kind == PendKey) {
      d = q.deck & 1, key = q.key;
      return true;
    }
    if (q.kind == PendCmd && (q.c.kind == CmdHandDown || q.c.kind == CmdHandUp)) {
      d = q.c.deck & 1, key = KeyMouse;
      return true;
    }
    if (q.kind == PendIn && q.e.type == TypeButton) {
      const int k = controlCmdKey(q.e.control);
      if (k < 0) return false;
      d = q.e.deck & 1, key = k;
      return true;
    }
    return false;
  }
  static bool pendDown(const Pend& q) {
    if (q.kind == PendKey) return q.down;
    if (q.kind == PendCmd) return q.c.kind == CmdHandDown;
    return q.e.down;
  }
  // Step start: the inputs that wait for it, in a fixed order (a replay does the same):
  // buttons by deck and key (a second press or release of one key waits for the next step),
  // the jog / rim / search ticks, nudges, seeks, the mouse; then the lanes of a replay.
  void runPending() {
    const bool rec = capturing_ && perfCapture_;
    std::array<std::array<bool, kCmdKeys>, kDecks> used{};
    size_t stop = pendTail_;
    for (; stop != pendHead_; ++stop) {
      int d = 0, key = 0;
      if (!pendKey(pend_[stop % pend_.size()], d, key)) continue;
      auto& u = used[static_cast<size_t>(d)][static_cast<size_t>(key)];
      if (u) break;
      u = true;
    }
    const double now = static_cast<double>(sh_.clock);
    // buttons
    for (int d = 0; d < kDecks; ++d)
      for (int key = 0; key < kCmdKeys; ++key) {
        if (!used[static_cast<size_t>(d)][static_cast<size_t>(key)]) continue;
        for (size_t t = pendTail_; t != stop; ++t) {
          const Pend& q = pend_[t % pend_.size()];
          int qd = 0, qk = 0;
          if (!pendKey(q, qd, qk) || qd != d || qk != key) continue;
          const bool down = pendDown(q);
          Deck& k = deck_[static_cast<size_t>(d)];
          if (k.driven_) break;  // (J-163: a pass plays the deck)
          sh_.evAt = q.at, sh_.evDelay = q.delay;
          if (key == KeyMouse) {
            if (down) k.handDown();
            else k.handUp();
          } else {
            InEvent e;
            e.control = static_cast<uint8_t>(cmdKeyControl(key));
            e.type = TypeButton;
            e.down = down;
            e.deck = static_cast<uint8_t>(d);
            k.input(e);
          }
          if (rec) {
            CaptureEvent m;
            m.kind = CaptureEvent::Button;
            m.step = static_cast<uint32_t>(capSteps_);
            m.deck = static_cast<uint8_t>(d);
            m.param = static_cast<uint8_t>(key);
            m.value = down ? 1.f : 0.f;
            pushCapture(m);
          }
          break;
        }
      }
    // ticks (routed with the state the buttons left), seeks, the mouse
    std::array<int, kDecks> nudge{}, search{};
    std::array<bool, kDecks> seekAny{}, mouseAny{};
    std::array<double, kDecks> seekTo{}, mouseTo{};
    for (size_t t = pendTail_; t != stop; ++t) {
      const Pend& q = pend_[t % pend_.size()];
      int qd = 0, qk = 0;
      if (pendKey(q, qd, qk) || q.kind == PendNone) continue;
      if (q.kind == PendIn) {
        const auto d = static_cast<size_t>(q.e.deck & 1);
        Deck& k = deck_[d];
        if (k.driven_) continue;
        sh_.evAt = q.at, sh_.evDelay = q.delay;
        switch (q.e.control) {
          case CtlJogPlatter:
            if (k.scr_ == Deck::ScrJog && !k.softHand_ && (k.hand_ || k.jogRelease_)) k.jogTicks(q.e.ticks * k.jogSens());
            else if (sh_.clock >= k.rimMuteUntil_ || !k.on(DVinyl)) nudge[d] += q.e.ticks;
            break;
          case CtlJogRim:
            if (!k.hand_ && sh_.clock >= k.rimMuteUntil_) nudge[d] += q.e.ticks;
            break;
          case CtlJogSearch: search[d] += q.e.ticks; break;
          default: k.input(q.e); break;
        }
      } else if (q.kind == PendCmd) {
        const auto d = static_cast<size_t>(q.c.deck & 1);
        if (deck_[d].driven_) continue;
        if (q.c.kind == CmdSeek) seekAny[d] = true, seekTo[d] = q.c.v;
        else if (q.c.kind == CmdHandMove) mouseAny[d] = true, mouseTo[d] = q.c.v;
      }
    }
    pendTail_ = stop;
    sh_.evAt = now, sh_.evDelay = 0;
    for (int di = 0; di < kDecks; ++di) {
      const auto d = static_cast<size_t>(di);
      Deck& k = deck_[d];
      const bool lanes = following_;
      // J-163: a pass's state lanes: where the hands acted (the travel lanes follow)
      if (k.driven_) k.drive(driveStep(di));
      const auto pPos = static_cast<size_t>(deckParam(di, DPos)), pMouse = static_cast<size_t>(deckParam(di, DMouse)),
                 pWheel = static_cast<size_t>(deckParam(di, DWheel)), pNudge = static_cast<size_t>(deckParam(di, DNudge));
      const bool hPos = lanes && !k.driven_ && sh_.laneHas[pPos], hMouse = lanes && sh_.laneHas[pMouse], hWheel = lanes && sh_.laneHas[pWheel],
                 hNudge = lanes && sh_.laneHas[pNudge];
      if (lanes && !primed_[d]) {  // following from the middle: the lanes' values are where it starts
        if (hPos) k.posN_ = static_cast<float>(sh_.laneN[pPos]);
        if (hMouse) k.mouseN_ = static_cast<float>(sh_.laneN[pMouse]);
        // (a pass picked up in the middle: its travel lanes count from the step before, so this
        // step's wheel turn and nudges are not lost)
        auto before = [&](size_t p) {
          const AutomationClip* c = k.driven_ ? laneClip_[p] : nullptr;
          const double ct = c ? laneC_[p] - ticksPerStep() : 0.0;
          return c && ct >= c->points.front().at() ? clipNorm(*c, automationValue(*c, ct)) : sh_.laneN[p];
        };
        if (hWheel) k.wheelN_ = static_cast<float>(before(pWheel));
        if (hNudge) k.nudgeSum_ = static_cast<int>(std::lround(valueOfN(static_cast<int>(pNudge), before(pNudge))));
        primed_[d] = true;
      }
      // nudges
      int dn = nudge[d];
      if (hNudge) {
        constexpr int span = static_cast<int>(kNudgeSpan);
        dn = static_cast<int>(std::lround(valueOfN(static_cast<int>(pNudge), sh_.laneN[pNudge]))) - k.nudgeSum_;
        while (dn >= span) dn -= 2 * span;
        while (dn < -span) dn += 2 * span;
      }
      if (dn != 0) k.nudgeTicks(dn);
      // seeks and search: where the record goes
      if (hPos) {
        const auto n = static_cast<float>(sh_.laneN[pPos]);
        if (n != k.posN_) k.seekLane(n);
      } else if (seekAny[d] || search[d] != 0) {
        double to = seekTo[d];
        if (search[d] != 0) to = k.pos_ + search[d] * k.srcRate() * 0.2;
        k.seek(to);
      }
      // the mouse
      if (hMouse) k.setMouse(sh_.laneN[pMouse]);
      else if (mouseAny[d]) k.handMove(mouseTo[d]);
      // the wheel (the jog filter takes it in the step)
      k.wheelLane_ = hWheel;
      if (hWheel) k.wheelLaneN_ = static_cast<float>(sh_.laneN[pWheel]);
    }
  }
  // A performance's start note: the decks go back to how they were when it was recorded,
  // on this very sample (a fresh step starts here, as it did then).
  void startPerformance(int id) {
    if (capturing_ || !sh_.aux || !sh_.transport) return;
    cut();
    while (pendTail_ != pendHead_ && pend_[pendTail_ % pend_.size()].stamp < sh_.clock) ++pendTail_;  // (what came before)
    for (size_t t = pendTail_; t != pendHead_; ++t)  // (presses the start state already holds)
      if (pend_[t % pend_.size()].held) pend_[t % pend_.size()].kind = PendNone;
    const PerfStart* ps = nullptr;
    for (const auto& st : sh_.aux->m.starts)
      if (st.id == id) ps = &st;
    if (takeOn_) stopTake();
    if (ps && ps->v.size() == snapCount()) {
      size_t i = 0;
      snapAll([&](double& x) { x = ps->v[i++]; });
      for (auto& d : deck_) d.perfRestored();
      // the controls without a lane keep the values they had in the pass until they change
      // here (a knob turned, automation elsewhere)
      startRaw_ = sh_.raw;
      startHeld_ = true;
      primed_.fill(true);
      primedByStart_ = true;
    }
    perfOn_ = true;
    lastStartAt_ = sh_.clock;
    updateStrict();
  }
  // The state a performance starts from: the mixer's ramp, the values the last step used,
  // both decks.
  template <class Fn>
  void snapAll(Fn&& f) {
    f(master0_), f(master1_);
    // (the parameters J-159 had: start states keep their layout; the state lanes are never in them)
    for (size_t i = 0; i < static_cast<size_t>(kLegacyPCount); ++i) {
      double v = lastP_[i];
      f(v);
      lastP_[i] = static_cast<float>(v);
    }
    for (size_t i = 0; i < static_cast<size_t>(kLegacyPCount); ++i) {
      double v = startN_[i];
      f(v);
      startN_[i] = static_cast<float>(v);
    }
    for (auto& d : deck_) d.snapFields(f);
  }
  size_t snapCount() {
    size_t n = 0;
    snapAll([&](double&) { ++n; });
    return n;
  }

  // A note lands inside a step: the step ends here and the next one starts on this sample.
  void cut() {
    if (stepPos_ <= 0 || stepPos_ >= stepLen_) {
      if (stepPos_ <= 0) stepLen_ = 0;
      return;
    }
    for (auto& d : deck_) d.cut(stepPos_);
    const double u = static_cast<double>(stepPos_) / stepLen_;
    master1_ = master0_ + (master1_ - master0_) * u;
    stepLen_ = stepPos_;
  }

  void runStep(float* L, float* R, int count) {
    float bl[kStep], br[kStep];
    std::fill(bl, bl + count, 0.f);
    std::fill(br, br + count, 0.f);
    for (auto& d : deck_) d.runStep(bl, br, stepPos_, count);
    const double fLen = static_cast<double>(stepLen_);
    for (int c = 0; c < count; ++c) {
      const double g = master0_ + (master1_ - master0_) * (static_cast<double>(stepPos_ + c + 1) / fLen);
      const auto l = static_cast<float>(bl[c] * g), r = static_cast<float>(br[c] * g);
      L[c] += l;
      R[c] += r;
      peakL_ = std::max(peakL_, std::fabs(l));
      peakR_ = std::max(peakR_, std::fabs(r));
    }
    stepPos_ += count;
    sh_.clock += static_cast<uint64_t>(count);
  }

  void publish() {
    for (auto& d : deck_) d.publish(capturing_, takeKey_);
    pubMasterL_.store(peakL_, std::memory_order_relaxed);
    pubMasterR_.store(peakR_, std::memory_order_relaxed);
    peakL_ = peakR_ = 0;
    pubTransport_.store(sh_.transport, std::memory_order_relaxed);
    pubReplay_.store(sh_.replay, std::memory_order_relaxed);
    pubSongBeat_.store(sh_.songBeat(static_cast<double>(sh_.clock - sh_.blockClock)), std::memory_order_relaxed);
    pubTempo_.store(sh_.tempo, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------- state
  Shared sh_;
  std::array<Deck, kDecks> deck_;
  std::array<float, PCount> lastState_{};
  bool recording_ = false;
  int stepPos_ = 0, stepLen_ = 0;
  double master0_ = 1, master1_ = 1;
  float peakL_ = 0, peakR_ = 0;
  struct Held {
    uint32_t id = 0;
    int key = -1;
  };
  std::array<Held, 32> held_{};

  // takes
  bool takeOn_ = false;
  int takeIndex_ = -1, takeKey_ = -1;
  size_t takeSteps_ = 0, moveAt_ = 0;
  double takeT_ = 0, takeStepOut_ = kStep;
  std::array<bool, kMoveParams> takeHas_{};
  std::array<float, kMoveParams> takeVal_{}, capVal_{};
  bool capturing_ = false, capLost_ = false;
  // performances (J-159)
  std::array<Pend, 512> pend_{};
  size_t pendHead_ = 0, pendTail_ = 0;
  bool perfCapture_ = false;  // the pass being captured is a performance
  bool perfOn_ = false;       // a start note played (follow until the position jumps)
  bool heldPress_ = false;    // the note being handled is a held-from-before press
  bool following_ = false;
  std::array<bool, kDecks> primed_{};
  bool primedByStart_ = false;
  uint64_t lastStartAt_ = 0, prevBlockClock_ = 0;
  const ParamLane* lanes_ = nullptr;
  int laneCount_ = 0;
  bool anchorOk_ = false;
  double anchorTempo_ = 0, anchorSr_ = 0;
  int anchorPpq_ = 0;
  std::array<float, PCount> lastP_{}, capN_{};
  // J-159: the controls' values where a performance starts (part of its start state) and the
  // stored values when its start note played: a control without a lane keeps the first
  // until the second changes
  std::array<float, PCount> startN_{}, startRaw_{};
  bool startHeld_ = false;
  int64_t wrapAt_ = -1;  // J-159: the sample the loop wraps on (this block), -1 none
  int64_t punchEndAt_ = -1;  // J-159: the sample recording punches out on (this block), -1 none
  bool punch_ = false;
  double punchIn_ = 0, punchOut_ = 0;
  double wrapStart_ = 0;
  std::array<double, 512> snapBuf_{};
  // J-163: the state lanes playing this step (their clip and content tick), each deck's pass
  std::array<double, PCount> capS_{};
  std::array<const AutomationClip*, PCount> laneClip_{};
  std::array<double, PCount> laneC_{};
  struct PassRef {
    bool on = false;
    int key = -1;  // the lane that names the pass (its first point)
    double g0 = 0, first = 0, lastC = 0;
    float tag = 0;
  };
  std::array<PassRef, kDecks> pass_{};
  std::array<bool, kDecks> chaseNow_{};
  std::array<double, DriveStep::kWarm> warmDx_{};  // (a chase in a jog scratch: the hand's last steps)
  int64_t driveAt_ = -1;  // the sample a pass is picked up on (-1 none)
  int64_t capStartAt_ = -1;
  double capStartTick_ = 0;
  uint64_t capSteps_ = 0, capSamples_ = 0;
  static constexpr size_t kCapture = 32768;
  std::vector<CaptureEvent> capture_;
  std::atomic<size_t> capHead_{0}, capTail_{0};

  std::atomic<KeyPool*> pending_{nullptr}, retired_{nullptr};
  double uiRate_ = 0;

  // inputs
  Decoder decoderHub_, decoderRaw_;
  uint64_t hubPos_ = 0;
  std::array<Cmd, 256> cmds_{};
  std::array<int64_t, 256> cmdNs_{};  // arrival times (hub::nowNs)
  std::atomic<size_t> cmdHead_{0}, cmdTail_{0};
  struct Timed {
    double at = 0;  // sample time it acts on
    InEvent e{};
  };
  std::array<Timed, 1024> timed_{};  // J-197: the controller's wheel input waiting for its sample
  size_t timedHead_ = 0, timedTail_ = 0;
  double lastBlock_ = 0;    // length of the last block (live jog ticks are laid out over the next one)
  int64_t blockAcc_ = 0;
  double lastArrival_ = 0;  // sample time of the last controller / editor input (they stay in order)
  double clockOff_ = 0;     // sample clock minus wall clock (samples)
  bool clockOffOk_ = false;

  // published
  std::atomic<float> pubMasterL_{0}, pubMasterR_{0};
  std::atomic<bool> pubTransport_{false};
  std::atomic<bool> pubReplay_{false};
  std::atomic<double> pubSongBeat_{0}, pubTempo_{120};
  std::atomic<uint32_t> raw_{0}, rawSeq_{0};
};

PlatterGen* setupOf(Generator* g) { return dynamic_cast<PlatterGen*>(g); }
const PlatterGen* setupOf(const Generator* g) { return dynamic_cast<const PlatterGen*>(g); }

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
  if (state.data.empty()) return true;
  std::string path;
  Model m;
  parseData(state.data, path, m);
  // every file the model refers to stays loaded (decks and takes)
  std::vector<SamplePtr> srcs(m.files.size());
  for (size_t i = 0; i < m.files.size(); ++i)
    if (!m.files[i].empty()) srcs[i] = sampleCache().get(m.files[i]);
  const int a = m.deck[0].file;
  if (a >= 0 && a < static_cast<int>(srcs.size())) aux.samples.assign(1, srcs[static_cast<size_t>(a)]);
  (void)pitch::table(pitch::Quality::Realtime);
  aux.custom = cachedAux(state.data, m, srcs);
  return true;
}

bool liveView(const Generator* g, int deck, LiveView& out) {
  const PlatterGen* d = setupOf(g);
  if (!d) return false;
  d->view(deck, out);
  return true;
}

bool mixView(const Generator* g, MixView& out) {
  const PlatterGen* d = setupOf(g);
  if (!d) return false;
  d->mixOut(out);
  return true;
}

bool stepState(const Generator* g, StepState& out) {
  const PlatterGen* d = setupOf(g);
  if (!d) return false;
  d->stepOut(out);
  return true;
}

bool mirror(const Generator* g, Mirror& out) {
  const PlatterGen* d = setupOf(g);
  if (!d) return false;
  d->mirrorOut(out);
  return true;
}

bool marks(const Generator* g, int deck, Marks& out) {
  const PlatterGen* d = setupOf(g);
  if (!d) return false;
  d->marksOut(deck, out);
  return true;
}

bool command(Generator* g, const Cmd& c) {
  PlatterGen* d = setupOf(g);
  return d && d->pushCmd(c);
}

int drainCapture(Generator* g, CaptureEvent* out, int max) {
  PlatterGen* d = setupOf(g);
  return d ? d->drain(out, max) : 0;
}

}  // namespace gl::packs::platter

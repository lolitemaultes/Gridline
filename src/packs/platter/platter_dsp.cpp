// Gridline — "platter" pack: the runtime (gl.platter): two decks and the mixer.
//
// Everything advances in control steps of kStep output samples (shorter when an
// event cuts a step), whatever the host block size, so the setup renders the
// same audio for any block size and a recorded take replays sample for sample.
// Both decks share the step grid: a note that cuts a step cuts it on both.
// Per step each deck decides where its read head ends (hand tracking, motor,
// loops, jumps) and its level; inside the step the head moves at a constant
// speed, every sample read through Gridline's band-limited kernel at that
// speed (any direction, down to zero), so a direction change is a continuous
// stop and turn, never a click. Jumps crossfade from a ghost reader that keeps
// the old motion for a few milliseconds.
//
// Physics (per deck):
//   hand    the platter follows the hand through an alpha-beta tracker (a
//           critically damped second-order loop; Hand feel sets its
//           bandwidth), which turns coarse jog ticks or mouse moves into a
//           smooth position and speed;
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
double masterGainOf(double db) { return db <= kMasterOffDb + 1e-3 ? 0.0 : dbToGain(db); }

// ================================================================== aux
struct PlatterAux final : AuxData {
  std::vector<SamplePtr> srcs;  // Model::files order (null = missing)
  Model m;
  uint32_t device = 0;  // hub id of the bound device (0 = any)
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
  bool transport = false;
  std::array<float, PCount> p{}, ctlVal{};
  std::array<uint64_t, PCount> ctlUntil{};
  std::array<std::atomic<uint32_t>, PCount> mirSeq{};
  std::array<std::atomic<float>, PCount> mirVal{};
  const PlatterAux* aux = nullptr;
  KeyPool* pool = nullptr;

  double songBeat(double sampleInBlock) const { return (blockTick + sampleInBlock * tempo * ppq / (60.0 * sr)) / static_cast<double>(ppq); }
  // A controller moved parameter p: it wins over the stored value for a moment
  // (the editor writes it into the parameter and the automation).
  void setCtl(int i, float v) {
    const auto k = static_cast<size_t>(i);
    ctlVal[k] = v;
    ctlUntil[k] = clock + static_cast<uint64_t>(sr * 0.25) + 1;
    p[k] = v;
    mirVal[k].store(v, std::memory_order_relaxed);
    mirSeq[k].fetch_add(1, std::memory_order_release);
  }
};

// ================================================================== one deck
struct Ghost {
  double pos = 0, rate = 0;
  int left = 0;
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
      motor_ = false, vel_ = 0, hand_ = false, softHand_ = false, ghost_.left = 0, loopOn_ = false;
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
      const double sb = sh_->songBeat(sampleInBlock);
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
    jumpPending_ = true;
    if (hand_) handPos_ = target, trX_ = target;
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
    if (instant) vel_ = baseSpeed();
  }
  void stopMotor(bool instant) {
    motor_ = false;
    if (instant) vel_ = 0;
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

  void handDown() {
    if (!loaded()) return;
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
    rimMuteUntil_ = sh_->clock + static_cast<uint64_t>(sh_->sr * 0.5);
  }
  double ticksPerRev() const { return std::max(8.0, sh_->aux ? sh_->aux->m.map.ticksPerRev : 128.0); }
  double framesPerTick() const { return srcRate() * kSecondsPerRev / ticksPerRev(); }
  void nudgeTicks(int ticks) {
    if (!loaded()) return;
    if (motor_ && !hand_) {
      nudge_ = std::clamp(nudge_ + ticks * 0.6 / ticksPerRev(), -0.5, 0.5);
    } else if (!motor_) {
      // stopped: the wheel moves the record by hand (it lets go after a moment)
      if (!hand_) {
        handDown();
        softHand_ = true;
      }
      handPos_ += ticks * framesPerTick() * 0.5;
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
          if (on(DSync) && motor_) {
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
        if (e.down && on(DVinyl)) handDown();
        else if (!e.down) handUp();
        break;
      case CtlJogPlatter:
        if (hand_ && !softHand_) handPos_ += e.ticks * framesPerTick();
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
      case CmdHandMove:
        if (hand_) handPos_ = handBase_ + c.v * srcRate() * kSecondsPerRev;
        break;
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
    } else if (noteHot_ == i) {
      noteHot_ = -1;
      if (static_cast<int>(std::lround(sh_->p[PNoteMode])) == NoteGate) {
        motor_ = false;
        vel_ = 0;
      }
    }
  }
  void allOff() {
    if (noteHot_ >= 0) hotKey(noteHot_, false);
  }
  void kill() {
    motor_ = false, hand_ = false, softHand_ = false, vel_ = 0, cuePreview_ = false, hotPreview_ = -1, playLatched_ = false, noteHot_ = -1;
    censor_ = false, bend_ = 0, nudge_ = 0;
    ghost_.left = 0;
  }
  bool active() const { return rs() && (motor_ || hand_ || std::fabs(vel_) > 1e-9 || ghost_.left > 0 || inTake_); }

  // ---------------------------------------------------------------- takes
  // The deck plays its stream of a take (null: it stays silent for the take).
  void startTake(const TakeStream* s, const Sample* src) {
    inTake_ = true;
    stream_ = s && s->on && src ? s : nullptr;
    takeSrc_ = stream_ ? src : nullptr;
    hand_ = softHand_ = false;
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
  // Level that follows the platter's speed (a record makes no sound standing still).
  double speedGain(double rate) const {
    const double r = std::fabs(rate) / std::max(1e-9, unity());
    return std::sqrt(std::min(1.0, r / 0.5));
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
    if (softHand_ && clock >= softUntil_) handUp(), vel_ = trV_ = 0;
    // search: whole jumps
    if (searchTicks_ != 0) {
      jump(pos_ + searchTicks_ * srcRate() * 0.2);
      searchTicks_ = 0;
    }
    // slip: the ghost position keeps time while something holds the record
    const bool slipNow = !inTake_ && on(DSlip) && (hand_ || censor_ || loopOn_ || hotPreview_ >= 0);
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
        start = jumpTo_;
        jumped = std::fabs(jumpTo_ - x0) > 1e-6;
        jumpPending_ = false;
      }
      if (hand_) {
        // alpha-beta tracker, critically damped, bandwidth from Hand feel (4 .. 16 Hz)
        const double f = 4.0 * std::pow(4.0, std::clamp(static_cast<double>(P(DFeel)), 0.0, 1.0));
        const double wT = 2 * kPi * f * fLen / sh_->sr;
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
    if (replay) wantKey = sh_->pool && (flags & TfKeyLock);
    else
      wantKey = sh_->pool && on(DKeyLock) && (flags & TfMotor) && !(flags & TfHand) && rel > 0.5 && rel < 1.9 && !censor_ &&
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
    Pl.sg0 = lastSpeedGain_, Pl.sg1 = speedGain(rate);
    Pl.hand = hand_;
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
        rd.cursor = static_cast<int64_t>(std::floor(start));
        pool->v[idx_][keyCur_].reset();
        pool->v[idx_][keyCur_].start(readFrames, &rd, rate, srcRate() / sr, false);
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
    pos_ = Pl.start + (Pl.x1 - Pl.start) * u;
    if (Pl.hand) trX_ = pos_;
    else if (!inTake_) vel_ = Pl.v0 + (Pl.v1 - Pl.v0) * u;
    lastGain_ = static_cast<float>(Pl.g0 + (Pl.g1 - Pl.g0) * u);
    lastSpeedGain_ = Pl.sg0 + (Pl.sg1 - Pl.sg0) * u;
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
      pool->v[idx_][keyCur_].render(out, count, Pl.rate, ratio, 0.0, readFrames, &pool->rd[idx_][keyCur_]);
    }
    const bool keyGhostNow = keyGhost_ >= 0 && ghost_.left > 0 && pool;
    if (keyGhostNow) {
      float* out[2] = {kg[0], kg[1]};
      pool->v[idx_][keyGhost_].render(out, count, Pl.ghostRate, ratio, 0.0, readFrames, &pool->rd[idx_][keyGhost_]);
    }
    const bool eqFlat = Pl.eqH == 1.0 && Pl.eqM == 1.0 && Pl.eqL == 1.0 && eqGain_[0] == 1.0 && eqGain_[1] == 1.0 && eqGain_[2] == 1.0;
    for (int c = 0; c < count; ++c) {
      const int j = from + c;
      const double u = static_cast<double>(j) / fLen;
      double l = 0, r = 0;
      if (Pl.key) {
        l = kl[0][c], r = kl[1][c];
      } else {
        const double p = Pl.start + (Pl.x1 - Pl.start) * u;
        if (pitch::exactFrame(p, Pl.rate)) {  // whole frames at normal speed: the file itself
          const auto k = static_cast<int64_t>(std::floor(p + 0.5));
          const bool in = k >= 0 && k < n;
          l = in ? xs[0][k] : 0.0, r = in ? xs[1][k] : 0.0;
        } else {
          float o[2];
          pitch::readArrays<2>(xs, n, p, std::max(std::fabs(Pl.rate), 1e-6), tab, o);
          l = o[0], r = o[1];
        }
      }
      const double sg = Pl.sg0 + (Pl.sg1 - Pl.sg0) * u;
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
    double start = 0, x1 = 0, rate = 0, v0 = 0, v1 = 0, sg0 = 0, sg1 = 0, ghostRate = 1;
    double g0 = 0, g1 = 0;
    const Sample* src = nullptr;  // the file the step reads (a take that ends mid-step keeps it to the step's end)
    double eqH = 1, eqM = 1, eqL = 1, trim = 1, wetT = 0, fg = 0.5, fk = 1.4, smooth = 0.01;
  };
  Plan plan_;
  float peak_ = 0, pre_ = 0;
  Ghost ghost_;
  double lastSpeedGain_ = 0;
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
  std::atomic<int> pubTake_{-1};
  std::atomic<float> pubLevel_{0}, pubPre_{0};
  std::atomic<uint32_t> marksSeq_{0};
};

// ================================================================== the setup (decks + mixer)
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

  // UI thread (Program build): the key lock voices for this sample rate.
  void prepareHost(const PluginState& state, double sampleRate) override {
    (void)state;
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    if (sampleRate == uiRate_) return;
    auto* p = new KeyPool;
    p->rate = sampleRate;
    for (auto& dv : p->v)
      for (auto& v : dv) v.prepare(2, sampleRate, stretch::Mode::Auto, 2.5, 2.5);
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
  void view(int d, LiveView& v) const {
    deck_[static_cast<size_t>(d & 1)].view(v);
    v.rawSeq = rawSeq_.load(std::memory_order_acquire);
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
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps&) override { onNoteOn(id, pitch, velocity); }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const int kind = status & 0xF0;
    noteRaw(status, d1, d2);
    if (hub::tapped()) {
      // the app reads devices from the hub: channel MIDI only plays keys (not
      // the setup's own controller buttons)
      if ((kind == 0x90 || kind == 0x80) && !boundNote(status, d1)) key(d1, kind == 0x90 && d2 > 0);
      return;
    }
    // plugin builds: the host's MIDI is the controller
    InEvent ev[16];
    const int n = sh_.aux ? decoderRaw_.feed(sh_.aux->m.map, status, d1, d2, ev, 16) : 0;
    for (int i = 0; i < n; ++i) input(ev[i]);
    if (n == 0 && (kind == 0x90 || kind == 0x80)) key(d1, kind == 0x90 && d2 > 0);
  }
  void onAllOff() override {
    for (auto& h : held_) h.key = -1;
    for (auto& d : deck_) d.allOff();
    if (takeOn_) stopTake();
  }
  void onKill() override {
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
    sh_.transport = ctx.playing;
    recording_ = ctx.recording && ctx.playing && !ctx.offline;
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
      sh_.p[k] = sh_.ctlUntil[k] > 0 ? sh_.ctlVal[k] : v;
    }
    // a take plays its own mixer moves
    if (takeOn_)
      for (int i = 0; i < kMoveParams; ++i) {
        const auto p = static_cast<size_t>(moveParams()[static_cast<size_t>(i)]);
        if (takeHas_[static_cast<size_t>(i)]) sh_.p[p] = takeVal_[static_cast<size_t>(i)];
      }
    // controller MIDI from the hub (app): the setup's device
    if (hub::tapped()) {
      const uint64_t head = hub::head();
      if (head - hubPos_ > hub::kSize) hubPos_ = head - hub::kSize;
      const uint32_t want = sh_.aux ? sh_.aux->device : 0;
      InEvent ev[16];
      for (; hubPos_ < head; ++hubPos_) {
        uint32_t dev = 0;
        uint8_t st = 0, a = 0, b = 0;
        if (!hub::read(hubPos_, dev, st, a, b)) continue;
        if (want != 0 && dev != want) continue;
        noteRaw(st, a, b);
        if (!sh_.aux) continue;
        const int n = decoderHub_.feed(sh_.aux->m.map, st, a, b, ev, 16);
        for (int i = 0; i < n; ++i) input(ev[i]);
      }
    } else {
      hubPos_ = hub::head();
    }
    // capture: begins on the next beat while Gridline records, ends when it stops or the position jumps
    const bool anyLoaded = deck_[0].loaded() || deck_[1].loaded();
    if (capturing_ && (!recording_ || ctx.jumped || !armed())) endCapture();
    if (recording_ && armed() && anyLoaded && !capturing_ && capStartAt_ < 0) {
      const double spt = sh_.sr * 60.0 / (sh_.tempo * sh_.ppq);  // samples per tick
      const double beat = static_cast<double>(sh_.ppq);
      const double tb = std::ceil(sh_.blockTick / beat - 1e-9) * beat;
      const double off = (tb - sh_.blockTick) * spt;
      capStartAt_ = static_cast<int64_t>(sh_.clock) + static_cast<int64_t>(std::ceil(off - 1e-9));
      capStartTick_ = tb;
    }
    if (!recording_) capStartAt_ = -1;
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
        if (static_cast<int64_t>(sh_.clock) >= capStartAt_) {
          cut();  // the take starts exactly here, on a fresh step
          beginCapture();
        }
      }
      if (stepPos_ >= stepLen_) computeStep();
      int run = std::min(n - i, stepLen_ - stepPos_);
      if (capStartAt_ >= 0 && !capturing_ && static_cast<int64_t>(sh_.clock) < capStartAt_)
        run = static_cast<int>(std::min<int64_t>(run, capStartAt_ - static_cast<int64_t>(sh_.clock)));
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
  void input(const InEvent& e) {
    if (takeOn_ && e.control != CtlShift && e.control != CtlNone && controlInfo(e.control).type != TypeAbsolute) stopTake();  // the hand takes over
    switch (e.control) {
      case CtlXfader: sh_.setCtl(PXfader, e.value); break;
      case CtlMaster: sh_.setCtl(PMaster, static_cast<float>(kMasterOffDb + e.value * (6.0 - kMasterOffDb))); break;
      default: deck_[static_cast<size_t>(e.deck & 1)].input(e); break;
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
    deck_[static_cast<size_t>(c.deck & 1)].command(c);
  }

  // Keys: 36..59 takes, 60..67 deck A hot cues, 72..79 deck B hot cues.
  void key(int k, bool down) {
    // a note (render() is split at every event) cuts the running step here, so
    // what it starts lands on its exact sample, whatever the block size
    if (!capturing_) cut();
    for (int d = 0; d < kDecks; ++d) {
      const int first = d == 0 ? kHotKeyFirst : kHotKeyFirstB;
      if (k >= first && k < first + kHotCues) {
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
        }
    }
  }

  void beginCapture() {
    capturing_ = true;
    capStartAt_ = -1;
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
      e.gain[d] = k.lastGain_;
      if (k.keyOn_) k.keyRestart_ = true;  // the key lock engine starts fresh with the take (replays match)
    }
    pushCapture(e);
    capSteps_ = 0;
    capSamples_ = 0;
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
    e.tick = capStartTick_ + static_cast<double>(capSamples_) * sh_.tempo * sh_.ppq / (60.0 * sh_.sr);
    pushCapture(e);
  }
  void pushCapture(const CaptureEvent& e) {
    const size_t h = capHead_.load(std::memory_order_relaxed);
    if (h - capTail_.load(std::memory_order_acquire) >= capture_.size() - 1) {
      capLost_ = true;
      return;
    }
    if (capLost_ && (e.kind == CaptureEvent::Point || e.kind == CaptureEvent::Move)) return;
    CaptureEvent& slot = capture_[h % capture_.size()];
    slot = e;
    if (capLost_ && e.kind == CaptureEvent::End) slot.kind = CaptureEvent::Lost;  // the editor drops a take with holes
    capHead_.store(h + 1, std::memory_order_release);
    if (e.kind != CaptureEvent::Point && e.kind != CaptureEvent::Move) capLost_ = false;
  }

  // ---------------------------------------------------------------- the step
  void computeStep() {
    const int len = kStep;
    stepLen_ = len;
    stepPos_ = 0;
    // a take: its mixer moves land on their steps
    double adv = 0;
    bool exact = false;
    if (takeOn_) {
      adv = static_cast<double>(len) / takeStepOut_;
      exact = std::fabs(takeStepOut_ - kStep) < 1e-9 && std::fabs(takeT_ - std::round(takeT_)) < 1e-9;
      applyMoves(std::floor(takeT_ + 1e-9));
    }
    // capture: mixer values that changed since the last step
    if (capturing_) {
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
      CaptureEvent e;
      e.kind = CaptureEvent::Point;
      for (int d = 0; d < kDecks; ++d) e.pt[d] = deck_[static_cast<size_t>(d)].pt_;
      pushCapture(e);
      ++capSteps_;
      capSamples_ += static_cast<uint64_t>(len);
    }
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
  std::atomic<size_t> cmdHead_{0}, cmdTail_{0};

  // published
  std::atomic<float> pubMasterL_{0}, pubMasterR_{0};
  std::atomic<bool> pubTransport_{false};
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

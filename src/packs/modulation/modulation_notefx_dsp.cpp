// Gridline — "modulation" pack: note processors (PluginKind::NoteFx).
// Each processor works on one transport segment at a time (see NoteFxContext)
// and keeps its own sample clock, advanced by the segment lengths, so timing
// is identical for any audio block size.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "../../engine/NoteFx.h"
#include "modulation.h"
#include "modulation_dsp.h"

namespace gl::packs::modulation {

namespace {

inline float prm(const PluginState& s, int k, float dflt = 0) {
  return static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : dflt;
}
inline int iprm(const PluginState& s, int k, int dflt = 0) {
  return static_cast<int>(std::lround(prm(s, k, static_cast<float>(dflt))));
}

// Deterministic pseudo-random numbers (per processor instance).
struct Rng {
  uint32_t s = 0x9E3779B9u;
  double next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<double>(s) / 4294967296.0;
  }
};

// Input note id → output notes (id, target) for processors that create or re-route notes.
struct IdMap {
  struct E {
    uint32_t in, out;
    int16_t target;
  };
  static constexpr int kCap = 512;
  E e[kCap];
  int n = 0;
  void add(uint32_t in, uint32_t out, int16_t target) {
    if (n < kCap) e[n++] = {in, out, target};
  }
  // Emits note-offs for every output of `in` at `offset` and forgets them.
  void release(uint32_t in, int offset, NoteBuffer& out) {
    for (int i = 0; i < n;) {
      if (e[i].in == in) {
        NoteMsg m;
        m.kind = NoteMsg::Off;
        m.offset = offset;
        m.id = e[i].out;
        m.target = e[i].target;
        out.push(m);
        e[i] = e[--n];
      } else {
        ++i;
      }
    }
  }
  void releaseAll(int offset, NoteBuffer& out) {
    for (int i = 0; i < n; ++i) {
      NoteMsg m;
      m.kind = NoteMsg::Off;
      m.offset = offset;
      m.id = e[i].out;
      m.target = e[i].target;
      out.push(m);
    }
    n = 0;
  }
};

NoteMsg noteOn(int offset, uint32_t id, float pitch, float vel, int16_t color, int16_t target = -1) {
  NoteMsg m;
  m.kind = NoteMsg::On;
  m.offset = offset;
  m.id = id;
  m.pitch = std::clamp(pitch, 0.0f, 127.0f);
  m.velocity = std::clamp(vel, 0.0f, 1.0f);
  m.color = color;
  m.target = target;
  return m;
}
NoteMsg noteOff(int offset, uint32_t id, int16_t target = -1) {
  NoteMsg m;
  m.kind = NoteMsg::Off;
  m.offset = offset;
  m.id = id;
  m.target = target;
  return m;
}

// Held-note list shared by the arpeggiator and the sequencer.
struct HeldNotes {
  struct N {
    uint32_t id;
    float pitch, vel;
    int16_t color;
    uint32_t order;
  };
  static constexpr int kCap = 32;
  N v[kCap];
  int n = 0;
  uint32_t seq = 0;
  void add(const NoteMsg& m) {
    remove(m.id);
    if (n < kCap) v[n++] = {m.id, m.pitch, m.velocity, m.color, seq++};
  }
  bool remove(uint32_t id) {
    for (int i = 0; i < n; ++i)
      if (v[i].id == id) {
        for (int j = i + 1; j < n; ++j) v[j - 1] = v[j];
        --n;
        return true;
      }
    return false;
  }
  bool has(uint32_t id) const {
    for (int i = 0; i < n; ++i)
      if (v[i].id == id) return true;
    return false;
  }
  // Sorted copy: by pitch (ascending) or in the order played. A stable
  // insertion sort in place: std::stable_sort takes a temporary buffer from the
  // heap (libstdc++ always does, even for two notes), on the audio thread.
  int sorted(N* out, bool byPitch) const {
    auto before = [byPitch](const N& a, const N& b) { return byPitch ? a.pitch < b.pitch : a.order < b.order; };
    for (int i = 0; i < n; ++i) {
      int j = i;
      for (; j > 0 && before(v[i], out[j - 1]); --j) out[j] = out[j - 1];
      out[j] = v[i];
    }
    return n;
  }
};

// Notes sounding from a stepped processor, each with its scheduled end.
struct Sounding {
  struct S {
    uint32_t id;
    int64_t offAt;  // processor clock
  };
  static constexpr int kCap = 32;
  S v[kCap];
  int n = 0;
  void add(uint32_t id, int64_t offAt) {
    if (n < kCap) v[n++] = {id, offAt};
  }
  void endAll(int offset, NoteBuffer& out) {
    for (int i = 0; i < n; ++i) out.push(noteOff(offset, v[i].id));
    n = 0;
  }
  int64_t nextOff() const {
    int64_t t = INT64_MAX;
    for (int i = 0; i < n; ++i) t = std::min(t, v[i].offAt);
    return t;
  }
  void endDue(int64_t clock, int offset, NoteBuffer& out) {
    for (int i = 0; i < n;) {
      if (v[i].offAt <= clock) {
        out.push(noteOff(offset, v[i].id));
        v[i] = v[--n];
      } else {
        ++i;
      }
    }
  }
};

// Base for processors driven by a step clock (arpeggiator, sequencer): walks
// a segment event by event in sample order.
//
// J-138: steps sit on the song's step grid while the transport plays. The first
// step sounds when the first key goes down; it counts as the grid step nearest
// to that moment, and the following steps fall on the grid from there, so a key
// pressed a little late or early still swings in time. Swing delays the second
// step of every pair (counted from the start of the pattern, see swingStep) by
// swing x half a step, read from the knob as each step is scheduled. When the
// song position jumps (loop, seek) the next step lands on the next grid step.
// With the transport stopped the grid starts at the first key.
class Stepper : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux& aux, const NoteFxContext& ctx, const NoteBuffer& in,
               NoteBuffer& out) override {
    (void)aux;
    state_ = &s;
    ctx_ = &ctx;
    const double stepTicks = std::max(1e-6, rateBeats(prm(s, rateParam(), 4)) * ctx.ppq);
    stepSamples_ = std::max(1.0, stepTicks * ctx.spt);
    int idx = 0;
    const int64_t segStart = clock_;
    const int64_t segEnd = clock_ + ctx.len;
    if (running_) {
      const double now = ctx.tickAt(ctx.from);
      if (ctx.playing != gridPlaying_ || std::fabs(now - expectTick_) > 0.5) {  // position jumped: back onto the grid
        const double left = std::max(0.0, slotTick_ - expectTick_);
        gridPlaying_ = ctx.playing;
        slotTick_ = ctx.playing ? std::ceil(now / stepTicks - 1e-9) * stepTicks : now + std::min(left, stepTicks);
        originTick_ = ctx.playing ? 0.0 : slotTick_;
      } else if (std::fabs(stepTicks - slotSize_) > 1e-9) {  // rate changed: the next step of the new grid
        const double last = slotTick_ - slotSize_;
        slotTick_ = originTick_ + (std::floor((last - originTick_) / stepTicks + 1e-6) + 1.0) * stepTicks;
      }
      slotSize_ = stepTicks;
      schedule(segStart, segStart);
    }
    for (;;) {
      int64_t tIn = idx < in.count ? segStart + (in.ev[idx].offset - ctx.from) : INT64_MAX;
      int64_t tOff = sounding_.nextOff();
      int64_t tStep = running_ ? nextStep_ : INT64_MAX;
      int64_t t = std::min(tIn, std::min(tOff, tStep));
      if (t >= segEnd) break;
      int off = ctx.from + static_cast<int>(t - segStart);
      if (tOff <= t) sounding_.endDue(t, off, out);  // ends first, so a step can retrigger
      while (idx < in.count && segStart + (in.ev[idx].offset - ctx.from) == t) {
        input(in.ev[idx], off, t, segStart, stepTicks, out);
        ++idx;
      }
      if (running_ && nextStep_ <= t) {
        step(off, t, out);
        ++stepIndex_;
        slotTick_ += stepTicks;
        schedule(segStart, t + 1);
      }
    }
    expectTick_ = ctx.tickAt(ctx.end());
    clock_ = segEnd;
  }
  void reset() override {
    held_.n = 0;
    phys_.n = 0;
    sounding_.n = 0;
    running_ = false;
    stepIndex_ = 0;
  }
  int position() const override { return running_ ? displayStep_ : -1; }

 protected:
  virtual int rateParam() const = 0;
  virtual int swingParam() const = 0;
  virtual bool latchOn() const { return false; }
  virtual bool passThru() const { return false; }
  virtual void step(int off, int64_t t, NoteBuffer& out) = 0;
  // Position of step k in its swing pairs (the second of each pair swings).
  virtual int64_t swingStep(int64_t k) const { return k; }

  // Sample (processor clock) of the step after the ones played: its grid tick plus swing.
  void schedule(int64_t segStart, int64_t earliest) {
    const double sw = swingStep(stepIndex_) % 2 == 1 ? std::clamp(static_cast<double>(prm(*state_, swingParam())), 0.0, 1.0) * 0.5 : 0.0;
    const double tick = slotTick_ + sw * slotSize_;
    nextStep_ = std::max(earliest, segStart + (ctx_->offsetOf(tick) - ctx_->from));
  }
  void start(int64_t t, int64_t segStart, double stepTicks) {
    const double now = ctx_->tickAt(ctx_->from + (t - segStart));
    running_ = true;
    stepIndex_ = 0;
    gridPlaying_ = ctx_->playing;
    slotSize_ = stepTicks;
    originTick_ = ctx_->playing ? 0.0 : now;
    slotTick_ = ctx_->playing ? std::round(now / stepTicks) * stepTicks : now;  // the grid step this key stands for
    nextStep_ = t;  // the first step sounds with the key
  }
  void input(const NoteMsg& m, int off, int64_t t, int64_t segStart, double stepTicks, NoteBuffer& out) {
    if (m.kind == NoteMsg::AllOff) {
      sounding_.endAll(off, out);
      held_.n = phys_.n = 0;
      running_ = false;
      out.push(m);
      return;
    }
    if (m.kind == NoteMsg::On) {
      bool latch = latchOn();
      if (latch && phys_.n == 0) held_.n = 0;  // a new chord replaces the latched one
      phys_.add(m);
      held_.add(m);
      if (passThru()) out.push(noteOn(off, m.id, m.pitch, m.velocity, m.color));
      if (!running_) start(t, segStart, stepTicks);
      return;
    }
    // note-off
    phys_.remove(m.id);
    if (passThru()) out.push(noteOff(off, m.id));
    if (!latchOn()) held_.remove(m.id);
    if (held_.n == 0) {
      sounding_.endAll(off, out);
      running_ = false;
    }
  }
  const PluginState* state_ = nullptr;
  const NoteFxContext* ctx_ = nullptr;
  HeldNotes held_, phys_;
  Sounding sounding_;
  bool running_ = false, gridPlaying_ = false;
  int64_t clock_ = 0, nextStep_ = 0, stepIndex_ = 0;
  double stepSamples_ = 1;
  double slotTick_ = 0, slotSize_ = 1, originTick_ = 0, expectTick_ = 0;  // song ticks (J-138)
  int displayStep_ = -1;
  Rng rng_;
};

// ------------------------------------------------------------- Arpeggiator
class Arpeggiator final : public Stepper {
 protected:
  int rateParam() const override { return ArRate; }
  int swingParam() const override { return ArSwing; }
  bool latchOn() const override { return prm(*state_, ArLatch) >= 0.5f; }
  void step(int off, int64_t t, NoteBuffer& out) override {
    const PluginState& s = *state_;
    int dir = iprm(s, ArDirection);
    int range = std::clamp(iprm(s, ArRange) + 1, 1, 4);
    int repeats = std::clamp(iprm(s, ArRepeat, 1), 1, 8);
    double gate = std::clamp(static_cast<double>(prm(s, ArGate, 0.5f)), 0.05, 1.0);
    float transpose = static_cast<float>(iprm(s, ArTranspose));
    HeldNotes::N notes[HeldNotes::kCap];
    int cnt = held_.sorted(notes, dir != AdOrder);
    if (cnt == 0) return;
    sounding_.endAll(off, out);  // previous step ends where the next begins (gate 100%)
    int64_t len = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(stepSamples_ * gate - 1e-9)));
    int velMode = iprm(s, ArVelMode);
    auto vel = [&](const HeldNotes::N& n, int64_t k) {
      if (velMode == 1) return prm(s, ArVelocity, 0.8f);
      if (velMode == 2) return k % std::max(1, cnt * range) == 0 ? std::min(1.0f, n.vel * 1.25f) : n.vel * 0.8f;
      return n.vel;
    };
    int64_t k = stepIndex_ / repeats;
    if (dir == AdChord) {
      for (int oct = 0; oct < range; ++oct)
        for (int i = 0; i < cnt; ++i) {
          const float v = vel(notes[i], k);  // J-138: accents follow the steps (they used to accent every chord)
          if (!(v > 0.0f)) continue;         // velocity 0 rests
          uint32_t id = ctx_->newId();
          out.push(noteOn(off, id, notes[i].pitch + 12.0f * oct + transpose, v, notes[i].color));
          sounding_.add(id, t + len);
        }
      displayStep_ = static_cast<int>(k % 16);
      return;
    }
    int L = cnt * range;
    int64_t idx;
    switch (dir) {
      case AdDown: idx = L - 1 - k % L; break;
      case AdUpDown: {
        int64_t period = L > 1 ? 2 * L - 2 : 1;
        int64_t p = k % period;
        idx = p < L ? p : period - p;
        break;
      }
      case AdDownUp: {
        int64_t period = L > 1 ? 2 * L - 2 : 1;
        int64_t p = k % period;
        idx = p < L ? L - 1 - p : p - (L - 1);
        break;
      }
      case AdRandom:
        if (stepIndex_ % repeats == 0) lastRandom_ = static_cast<int64_t>(rng_.next() * L) % L;
        idx = lastRandom_;
        break;
      default: idx = k % L; break;  // up, as played
    }
    const HeldNotes::N& n = notes[idx % cnt];
    float pitch = n.pitch + 12.0f * static_cast<float>(idx / cnt) + transpose;
    displayStep_ = static_cast<int>(idx);
    const float v = vel(n, k);
    if (!(v > 0.0f)) return;  // velocity 0 (fixed velocity 0, or a key played at 0): the step rests
    uint32_t id = ctx_->newId();
    out.push(noteOn(off, id, pitch, v, n.color));
    sounding_.add(id, t + len);
  }

 private:
  int64_t lastRandom_ = 0;
};

// ---------------------------------------------------------- Note Sequencer
class NoteSequencer final : public Stepper {
 protected:
  int rateParam() const override { return SqRate; }
  int swingParam() const override { return SqSwing; }
  bool passThru() const override { return prm(*state_, SqThru) >= 0.5f; }
  int length() const { return std::clamp(iprm(*state_, SqLength, 16), 1, kSeqSteps); }
  // Swing pairs restart with every pass of the pattern (J-138: odd lengths keep step 1 straight).
  int64_t swingStep(int64_t k) const override { return k % length(); }
  void step(int off, int64_t t, NoteBuffer& out) override {
    const PluginState& s = *state_;
    int length = this->length();
    int st = static_cast<int>(stepIndex_ % length);
    displayStep_ = st;
    bool on = prm(s, seqParam(st, SqOn), 1) >= 0.5f;
    bool tie = prm(s, seqParam(st, SqTie)) >= 0.5f;
    // J-92: a note whose next step is a tie lasts past that step's start (it used
    // to end first, so a tie never had anything to hold).
    const bool nextTie = prm(s, seqParam((st + 1) % length, SqTie)) >= 0.5f;
    const int64_t through = static_cast<int64_t>(std::ceil(stepSamples_)) + 1;
    if (tie && sounding_.n > 0) {  // hold the previous note(s) through this step
      int64_t len = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(stepSamples_ * prm(s, seqParam(st, SqGate), 0.5f) - 1e-9)));
      if (nextTie) len = std::max(len, through);
      for (int i = 0; i < sounding_.n; ++i) sounding_.v[i].offAt = t + len;
      return;
    }
    sounding_.endAll(off, out);
    if (!on) return;
    HeldNotes::N notes[HeldNotes::kCap];
    int cnt = held_.sorted(notes, true);
    if (cnt == 0) return;
    int sel = iprm(s, seqParam(st, SqNote));
    float shift = 12.0f * static_cast<float>(iprm(s, seqParam(st, SqOctave))) + static_cast<float>(iprm(s, SqTranspose));
    // J-138: step velocity scales the played velocity (accents follow how hard the keys
    // are played), or replaces it (SvStep, the behaviour of states saved before the choice).
    const float stepVel = prm(s, seqParam(st, SqVelocity), 0.8f);
    const bool scale = iprm(s, SqVelMode, SvStep) == SvScale;
    int64_t len = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(stepSamples_ * prm(s, seqParam(st, SqGate), 0.5f) - 1e-9)));
    if (nextTie) len = std::max(len, through);
    auto play = [&](const HeldNotes::N& n) {
      const float vel = scale ? stepVel * n.vel : stepVel;
      if (!(vel > 0.0f)) return;  // velocity 0: the step rests (no silent note to hold a voice)
      uint32_t id = ctx_->newId();
      out.push(noteOn(off, id, n.pitch + shift, vel, n.color));
      sounding_.add(id, t + len);
    };
    if (sel == 8) {
      for (int i = 0; i < cnt; ++i) play(notes[i]);
    } else if (sel == 6) {
      play(notes[0]);
    } else if (sel == 7) {
      play(notes[cnt - 1]);
    } else if (sel == 9) {
      play(notes[static_cast<int>(rng_.next() * cnt) % cnt]);
    } else {
      play(notes[sel % cnt]);
    }
  }
};

// Shared clock for processors that delay notes (chord strum).
class ChordGenerator final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux&, const NoteFxContext& ctx, const NoteBuffer& in,
               NoteBuffer& out) override {
    int64_t segStart = clock_, segEnd = clock_ + ctx.len;
    int idx = 0;
    for (;;) {
      int64_t tIn = idx < in.count ? segStart + (in.ev[idx].offset - ctx.from) : INT64_MAX;
      int64_t tPend = INT64_MAX;
      for (int i = 0; i < pend_; ++i) tPend = std::min(tPend, pending_[i].at);
      int64_t t = std::min(tIn, tPend);
      if (t >= segEnd) break;
      int off = ctx.from + static_cast<int>(t - segStart);
      for (int i = 0; i < pend_;) {  // strummed notes now due
        if (pending_[i].at <= t) {
          out.push(noteOn(off, pending_[i].out, pending_[i].pitch, pending_[i].vel, pending_[i].color));
          map_.add(pending_[i].in, pending_[i].out, -1);
          pending_[i] = pending_[--pend_];
        } else {
          ++i;
        }
      }
      while (idx < in.count && segStart + (in.ev[idx].offset - ctx.from) == t) {
        const NoteMsg& m = in.ev[idx++];
        if (m.kind == NoteMsg::On) {
          chord(s, ctx, m, t, off, out);
        } else if (m.kind == NoteMsg::Off) {
          for (int i = 0; i < pend_;)  // not yet strummed: never sounds
            if (pending_[i].in == m.id)
              pending_[i] = pending_[--pend_];
            else
              ++i;
          map_.release(m.id, off, out);
        } else {
          pend_ = 0;
          map_.releaseAll(off, out);
          out.push(m);
        }
      }
    }
    clock_ = segEnd;
  }
  void reset() override {
    map_.n = 0;
    pend_ = 0;
  }

 private:
  void chord(const PluginState& s, const NoteFxContext& ctx, const NoteMsg& m, int64_t t, int off, NoteBuffer& out) {
    float notes[8];
    int cnt = chordVoicing(s, notes);
    float root = m.pitch + static_cast<float>(iprm(s, ChTranspose));
    double strum = prm(s, ChStrum);
    int dir = iprm(s, ChStrumDir);
    double vs = prm(s, ChVelSpread);
    int order[8];
    for (int i = 0; i < cnt; ++i) order[i] = dir == 1 ? cnt - 1 - i : i;
    if (dir == 2)
      for (int i = cnt - 1; i > 0; --i) std::swap(order[i], order[static_cast<int>(rng_.next() * (i + 1)) % (i + 1)]);
    for (int k = 0; k < cnt; ++k) {
      int i = order[k];
      float vel = static_cast<float>(m.velocity * (1.0 - vs * k / std::max(1, cnt)));
      uint32_t id = ctx.newId();
      int64_t delay = static_cast<int64_t>(std::llround(strum * k * ctx.sampleRate));
      if (delay <= 0) {
        out.push(noteOn(off, id, root + notes[i], vel, m.color));
        map_.add(m.id, id, -1);
      } else if (pend_ < kPend) {
        pending_[pend_++] = {m.id, id, t + delay, root + notes[i], vel, m.color};
      }
    }
  }
  struct Pend {
    uint32_t in, out;
    int64_t at;
    float pitch, vel;
    int16_t color;
  };
  static constexpr int kPend = 128;
  Pend pending_[kPend];
  int pend_ = 0;
  IdMap map_;
  int64_t clock_ = 0;
  Rng rng_;
};

// --------------------------------------------------------------- Key Mapper
class KeyMapper final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux&, const NoteFxContext& ctx, const NoteBuffer& in,
               NoteBuffer& out) override {
    int root = iprm(s, KmRoot);
    const auto& sc = scales();
    unsigned mask = sc[static_cast<size_t>(std::clamp(iprm(s, KmScale), 0, static_cast<int>(sc.size()) - 1))].mask;
    int snap = iprm(s, KmSnap);
    float shift = static_cast<float>(iprm(s, KmTranspose) + 12 * iprm(s, KmOctave));
    bool both = iprm(s, KmMulti) == 1;
    auto inScale = [&](int p) { return (mask >> (((p - root) % 12 + 12) % 12)) & 1u; };
    for (int i = 0; i < in.count; ++i) {
      const NoteMsg& m = in.ev[i];
      if (m.kind == NoteMsg::On) {
        int pc = ((static_cast<int>(std::lround(m.pitch)) % 12) + 12) % 12;
        int ofs = iprm(s, KmMap + pc);
        if (ofs <= -13) continue;  // muted key
        float frac = m.pitch - std::round(m.pitch);
        int p = static_cast<int>(std::lround(m.pitch)) + ofs + static_cast<int>(shift);
        if (snap > 0 && !inScale(p)) {
          int best = p;
          for (int d = 1; d < 12; ++d) {
            if ((snap == 1 || snap == 2) && inScale(p + d)) {
              best = p + d;
              break;
            }
            if ((snap == 1 || snap == 3) && inScale(p - d)) {
              best = p - d;
              break;
            }
          }
          p = best;
        }
        float mapped = static_cast<float>(p) + frac;
        if (both) {
          out.push(m);  // original keeps its id
          if (std::fabs(mapped - m.pitch) > 0.01f) {
            uint32_t id = ctx.newId();
            out.push(noteOn(m.offset, id, mapped, m.velocity, m.color));
            map_.add(m.id, id, -1);
          }
        } else {
          out.push(noteOn(m.offset, m.id, mapped, m.velocity, m.color));
        }
      } else if (m.kind == NoteMsg::Off) {
        out.push(m);
        map_.release(m.id, m.offset, out);
      } else {
        map_.releaseAll(m.offset, out);
        out.push(m);
      }
    }
  }
  void reset() override { map_.n = 0; }

 private:
  IdMap map_;
};

// ------------------------------------------------------------- Key Splitter
class KeySplitter final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux& aux, const NoteFxContext& ctx, const NoteBuffer& in,
               NoteBuffer& out) override {
    bool first = iprm(s, SpMode) == 1;
    for (int i = 0; i < in.count; ++i) {
      const NoteMsg& m = in.ev[i];
      if (m.kind == NoteMsg::On) {
        for (int z = 0; z < kSplitZones; ++z) {
          if (prm(s, splitParam(z, SpOn)) < 0.5f) continue;
          float lo = prm(s, splitParam(z, SpLow)), hi = prm(s, splitParam(z, SpHigh), 127);
          float vlo = prm(s, splitParam(z, SpVelLow)), vhi = prm(s, splitParam(z, SpVelHigh), 1);
          if (m.pitch < lo - 0.5f || m.pitch > hi + 0.5f || m.velocity < vlo - 1e-6f || m.velocity > vhi + 1e-6f) continue;
          int16_t target = static_cast<int16_t>(aux.targets[z]);
          uint32_t id = ctx.newId();
          out.push(noteOn(m.offset, id, m.pitch + static_cast<float>(iprm(s, splitParam(z, SpTranspose))), m.velocity, m.color,
                          target));
          map_.add(m.id, id, target);
          if (first) break;
        }
      } else if (m.kind == NoteMsg::Off) {
        map_.release(m.id, m.offset, out);
      } else {
        map_.releaseAll(m.offset, out);
        out.push(m);
      }
    }
  }
  void reset() override { map_.n = 0; }

 private:
  IdMap map_;
};

// ------------------------------------------------------------- Level Scaler
}  // namespace

double vsRefCurve(double x, double c, double m, double o, double tension) {
  const double d = x - c, M = std::max(c, 1.0 - c);
  const double u = M > 0 ? std::fabs(d) / M : 0.0;
  const double k = (tension < 0 ? -1.0 : 1.0) * (std::pow(8193.0, std::fabs(tension)) - 1.0);
  double g = u;
  if (k > 0) g = 1.0 - (std::pow(k + 1.0, 1.0 - u) - 1.0) / k;
  else if (k < 0) g = (std::pow(1.0 - k, u) - 1.0) / -k;
  const double sgn = d > 0 ? 1.0 : d < 0 ? -1.0 : 0.0;
  return std::clamp(c + o + sgn * g * m * M, 0.0, 1.0);
}

namespace {

class LevelScaler final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux&, const NoteFxContext&, const NoteBuffer& in,
               NoteBuffer& out) override {
    if (iprm(s, VsModel) == 1) {
      processRef(s, in, out);
      return;
    }
    bool pitch = iprm(s, VsTarget) == 1;
    double center = prm(s, VsCenter), mul = prm(s, VsMultiply, 1), ofs = prm(s, VsOffset), ten = prm(s, VsTension);
    double hum = prm(s, VsHumanize);
    int hmode = iprm(s, VsHumanMode, 1);
    double lo = prm(s, VsMin), hi = prm(s, VsMax, 1);
    if (lo > hi) std::swap(lo, hi);
    for (int i = 0; i < in.count; ++i) {
      NoteMsg m = in.ev[i];
      if (m.kind == NoteMsg::On) {
        double x = pitch ? m.pitch / 127.0 : m.velocity;
        double y = center + (tensionCurve(x, ten) - center) * mul + ofs;
        if (hum > 0) {
          double r = rng_.next();
          y += hmode == 0 ? -hum * r : (hmode == 2 ? hum * r : hum * (2.0 * r - 1.0));
        }
        y = std::clamp(y, lo, hi);
        if (pitch)
          m.pitch = static_cast<float>(std::clamp(y * 127.0, 0.0, 127.0));
        else
          m.velocity = static_cast<float>(y);
      }
      out.push(m);
    }
  }
  void reset() override { rng_ = Rng{}; }

 private:
  // J-45 reference model: each active target (anything but multiply 1, offset 0,
  // tension 0, humanize 0) maps its value through vsRefCurve over its range;
  // humanize picks a point between the result and the range bound (scaled by
  // the distance to it), below (subtract), around (spread) or above (add).
  void processRef(const PluginState& s, const NoteBuffer& in, NoteBuffer& out) {
    for (int i = 0; i < in.count; ++i) {
      NoteMsg m = in.ev[i];
      if (m.kind == NoteMsg::On)
        for (int t = 0; t < kVsTargets; ++t) {
          const double c = prm(s, vsRef(t, VfCenter), t < 2 ? 0.f : 0.5f), mul = prm(s, vsRef(t, VfMultiply), 1),
                       o = prm(s, vsRef(t, VfOffset)), ten = prm(s, vsRef(t, VfTension)), J = prm(s, vsRef(t, VfHumanize));
          if (mul == 1 && o == 0 && ten == 0 && J == 0) continue;
          const double lo = t < 2 ? 0.0 : -1.0, hi = 1.0;
          const double key = std::nearbyint(m.pitch);
          double v = t == 0 ? m.velocity : t == 1 ? m.release : t == 2 ? (m.pitch - key) / 12.0 : t == 3 ? m.pan : t == 4 ? m.modX * 2.0 - 1.0 : m.modY * 2.0 - 1.0;
          const double x = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
          double r = std::clamp(lo + vsRefCurve(x, c, mul, o, ten) * (hi - lo), lo, hi);
          if (J > 0) {
            const int mode = std::clamp(iprm(s, vsRef(t, VfMode), 1), 0, 2);
            const double down = r - J * (r - lo), up = r + J * (hi - r);
            const double B = mode == 2 ? r : down, A = mode == 0 ? r : up;
            r = B + rng_.next() * (A - B);
          }
          const float f = static_cast<float>(r);
          switch (t) {
            case 0: m.velocity = f; break;
            case 1: m.release = f; break;
            case 2: m.pitch = static_cast<float>(key + r * 12.0); break;
            case 3: m.pan = f; break;
            case 4: m.modX = static_cast<float>((r + 1.0) * 0.5); break;
            default: m.modY = static_cast<float>((r + 1.0) * 0.5); break;
          }
        }
      out.push(m);
    }
  }
  Rng rng_;
};

// ------------------------------------------------------------ Colour Router
class ColourRouter final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux& aux, const NoteFxContext&, const NoteBuffer& in,
               NoteBuffer& out) override {
    int src = iprm(s, CmSource);
    for (int i = 0; i < in.count; ++i) {
      const NoteMsg& m = in.ev[i];
      if (m.kind == NoteMsg::On) {
        int g;
        switch (src) {
          case CsVelocity: g = std::clamp(static_cast<int>(m.velocity * 16.0f), 0, 15); break;
          case CsPitchClass: g = ((static_cast<int>(std::lround(m.pitch)) % 12) + 12) % 12; break;
          case CsOctave: g = std::clamp(static_cast<int>(std::lround(m.pitch)) / 12, 0, 15); break;
          default: g = m.color & 15; break;
        }
        int dest = iprm(s, CmMap + g);
        if (dest >= 9) continue;  // muted group
        int16_t target = dest == 0 ? int16_t{-1} : static_cast<int16_t>(aux.targets[dest - 1]);
        NoteMsg o = m;
        o.target = target;
        out.push(o);
        map_.add(m.id, m.id, target);
      } else if (m.kind == NoteMsg::Off) {
        map_.release(m.id, m.offset, out);
      } else {
        map_.releaseAll(m.offset, out);
        out.push(m);
      }
    }
  }
  void reset() override { map_.n = 0; }

 private:
  IdMap map_;
};

// --------------------------------------------------------------- Note Script
struct NoteScriptAux final : AuxData {
  NoteScriptProgram prog;
};
enum ScriptVar { SvPitch, SvVel, SvColor, SvA, SvB, SvC, SvBeat, SvT, SvN, SvCount };

class NoteScriptFx final : public NoteFx {
 public:
  void process(const PluginState& s, const NoteFxAux& aux, const NoteFxContext& ctx, const NoteBuffer& in,
               NoteBuffer& out) override {
    const auto* sa = dynamic_cast<const NoteScriptAux*>(aux.custom.get());
    bool enabled = prm(s, NsEnable, 1) >= 0.5f;
    for (int i = 0; i < in.count; ++i) {
      NoteMsg m = in.ev[i];
      if (m.kind == NoteMsg::On && enabled && sa) {
        double v[SvCount] = {m.pitch, m.velocity, static_cast<double>(m.color), prm(s, NsA, 0.5f), prm(s, NsB, 0.5f),
                             prm(s, NsC, 0.5f), ctx.tickAt(m.offset) / std::max(1, ctx.ppq),
                             static_cast<double>(clock_ + m.offset - ctx.from) / ctx.sampleRate, static_cast<double>(count_)};
        ++count_;
        const NoteScriptProgram& p = sa->prog;
        if (!p.play.empty() && p.play.eval(v, &rng_) < 0.5) {
          dropped_[dropN_++ % kDrop] = m.id;
          continue;
        }
        if (!p.pitch.empty()) m.pitch = static_cast<float>(std::clamp(p.pitch.eval(v, &rng_), 0.0, 127.0));
        if (!p.velocity.empty()) m.velocity = static_cast<float>(std::clamp(p.velocity.eval(v, &rng_), 0.0, 1.0));
      } else if (m.kind == NoteMsg::Off) {
        bool drop = false;
        for (int k = 0; k < kDrop; ++k)
          if (dropped_[k] == m.id && m.id != 0) {
            dropped_[k] = 0;
            drop = true;
          }
        if (drop) continue;
      }
      out.push(m);
    }
    clock_ += ctx.len;
  }
  void reset() override {
    count_ = 0;
    std::memset(dropped_, 0, sizeof dropped_);
  }

 private:
  static constexpr int kDrop = 128;
  uint32_t dropped_[kDrop] = {};
  int dropN_ = 0;
  int64_t clock_ = 0;
  uint64_t count_ = 0;
  uint32_t rng_ = 0xC0FFEEu;
};

}  // namespace

int chordVoicing(const PluginState& s, float* notes) {
  const auto& shapes = chordShapes();
  const ChordShape& sh = shapes[static_cast<size_t>(std::clamp(iprm(s, ChType), 0, static_cast<int>(shapes.size()) - 1))];
  int cnt = 0;
  for (int i = 0; i < sh.count; ++i) notes[cnt++] = static_cast<float>(sh.notes[i]);
  int inv = std::clamp(iprm(s, ChInversion), 0, 3);
  for (int k = 0; k < inv && k < cnt; ++k) {  // move the lowest note up an octave
    std::sort(notes, notes + cnt);
    notes[0] += 12;
  }
  std::sort(notes, notes + cnt);
  switch (iprm(s, ChSpread)) {
    case 1:  // drop 2: second-highest down an octave
      if (cnt >= 3) notes[cnt - 2] -= 12;
      break;
    case 2:  // open: every other note up an octave
      for (int i = 1; i < cnt; i += 2) notes[i] += 12;
      break;
    case 3:  // wide: each note a further octave apart
      for (int i = 1; i < cnt; ++i) notes[i] += 12.0f * static_cast<float>(i / 2 + (i % 2));
      break;
    default: break;
  }
  if (iprm(s, ChBass) == 1 && cnt < 8) notes[cnt++] = -12;
  std::sort(notes, notes + cnt);
  return cnt;
}

const char* defaultNoteScript() {
  return "pitch = pitch\nvelocity = vel\nplay = 1\n";
}

const std::vector<std::string>& noteScriptVars() {
  static const std::vector<std::string> v = {"pitch", "vel", "color", "a", "b", "c", "beat", "t", "n"};
  return v;
}

void compileNoteScript(const std::string& body, NoteScriptProgram& out) {
  out = NoteScriptProgram{};
  std::string text = body.empty() ? defaultNoteScript() : body;
  size_t p = 0;
  int line = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    std::string ln = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? text.size() : e + 1;
    ++line;
    size_t eq = ln.find('=');
    if (ln.find_first_not_of(" \t\r") == std::string::npos || ln[ln.find_first_not_of(" \t\r")] == '#') continue;
    if (eq == std::string::npos) {
      if (out.error.empty()) out.error = "line " + std::to_string(line) + ": expected name = formula";
      continue;
    }
    std::string name = ln.substr(0, eq);
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    std::string formula = ln.substr(eq + 1);
    Expr* target = name == "pitch" ? &out.pitch : name == "velocity" || name == "vel" ? &out.velocity
                                                 : name == "play" ? &out.play : nullptr;
    if (!target) {
      if (out.error.empty()) out.error = "line " + std::to_string(line) + ": unknown output '" + name + "'";
      continue;
    }
    std::string err;
    if (!target->compile(formula, noteScriptVars(), &err) && out.error.empty())
      out.error = "line " + std::to_string(line) + ": " + err;
  }
}

}  // namespace gl::packs::modulation

namespace gl {

std::unique_ptr<NoteFx> createNoteFx(const std::string& type) {
  using namespace packs::modulation;
  if (type == "gl.arp") return std::make_unique<Arpeggiator>();
  if (type == "gl.chord") return std::make_unique<ChordGenerator>();
  if (type == "gl.notesq") return std::make_unique<NoteSequencer>();
  if (type == "gl.keymap") return std::make_unique<KeyMapper>();
  if (type == "gl.split") return std::make_unique<KeySplitter>();
  if (type == "gl.velscale") return std::make_unique<LevelScaler>();
  if (type == "gl.colormap") return std::make_unique<ColourRouter>();
  if (type == "gl.notescript") return std::make_unique<NoteScriptFx>();
  return nullptr;
}

void prepareNoteFxAux(const PluginState& state, NoteFxAux& aux) {
  using namespace packs::modulation;
  if (state.type == "gl.notescript") {
    auto a = std::make_shared<NoteScriptAux>();
    compileNoteScript(noteFxBody(state.data), a->prog);
    aux.custom = a;
  }
}

}  // namespace gl

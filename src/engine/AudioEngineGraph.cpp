// Gridline — one block of the generator → mixer graph: channel tasks render
// instruments, mixer tasks pull their inputs (with delay compensation), run
// their effects and strip. Tasks run serially in Program::graph.serial order or
// on the worker pool following the graph's dependencies; every task writes
// only its own buffers and pulls inputs in a fixed order, so both produce
// identical output.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include "AudioEngine.h"
#include "SampleTap.h"
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

namespace gl {

namespace {
inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}
inline void storeMax(std::atomic<float>& a, float v) {
  float cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}
inline float peakOf(const float* x, int n) {
  float p = 0;
  for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}
inline float sumSq(const float* x, int n) {
  float a = 0;
  for (int i = 0; i < n; ++i) a += x[i] * x[i];
  return a;
}
// K-64: stops at the first sample that is not exact silence.
inline bool allZero(const float* x, int n) {
  for (int i = 0; i < n; ++i)
    if (x[i] != 0.f) return false;
  return true;
}
inline uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// K-317: the control blocks a block's strip gains ramp through. A stepped block (BlockCtx::steps > 1)
// starts on the grid and has one per kControlBlock frames (the last may be shorter), each ramping to
// its own target as a block of its own would; any other block is one piece from gpos.
struct GainPiece {
  int gpos, off, len;
};
inline GainPiece gainPiece(int j, int steps, int gpos, int n) {
  if (steps <= 1) {
    return {gpos, 0, n};
  }
  const int off = j * kControlBlock;
  return {0, off, std::min(kControlBlock, n - off)};
}
// The ramp's state after one piece (as the grid ramp leaves it).
inline void rampState(float& cur, float& from, int gpos, int len, float to) {
  if (gpos == 0) {
    from = cur;
  }
  const float k = static_cast<float>(gpos + len) / static_cast<float>(kControlBlock);
  cur = gpos + len >= kControlBlock ? to : from + (to - from) * k;
}
// A strip's ramp state over a block in which nothing is heard through it (the gains move all the same).
template <class Target>
void idleRamp(float& curL, float& curR, float& fromL, float& fromR, bool& primed, bool grid, int steps, int gpos, int n, Target target) {
  float tL = 0, tR = 0;
  int j = 0;
  if (!primed) {
    target(0, tL, tR);
    curL = tL, curR = tR;
    primed = true;
    if (steps <= 1) {
      return;
    }
    j = 1;
  }
  if (!grid) {
    target(0, tL, tR);
    curL = tL, curR = tR;
    return;
  }
  for (; j < std::max(1, steps); ++j) {
    const GainPiece pc = gainPiece(j, steps, gpos, n);
    target(j, tL, tR);
    rampState(curL, fromL, pc.gpos, pc.len, tL);
    rampState(curR, fromR, pc.gpos, pc.len, tR);
  }
}
}  // namespace

void AudioEngine::setThreads(int n) {
  n = std::clamp(n, 1, WorkerPool::kMaxThreads);
  pool_.setRealtime(!offline_);  // (setOffline comes first in the renders)
  pool_.ensureThreads(n - 1);
  threads_.store(n, std::memory_order_relaxed);
}

int AudioEngine::outputLatency() const { return current_ ? current_->outputLatency : 0; }

void AudioEngine::runGraph(const Program* p, int n) {
  const ProgGraph& g = p->graph;
  int threads = std::min(threads_.load(std::memory_order_relaxed), pool_.threads() + 1);
  // K-277: realtime playback wakes only as many pool threads as the block has heavy tasks to share:
  // the tasks that took a while in the last blocks, or channels given events now. Waiting workers
  // spin until the graph is done, so one heavy plugin among light tasks (or a silent project) on
  // every thread cost each of them that plugin's time. Offline renders use every thread.
  // GitHub #66: and only while the graph's work keeps the gate open (ParallelGate): an idle or light
  // graph runs on the audio thread alone and the workers sleep on their semaphore. The measured work
  // is of the blocks before (the last one's tasks may have run on several threads: their sum).
  if (!offline_ && threads > 1) {
    const double blockNs = 1e9 * n / std::max(1.0, sr_);
    heavyNs_ = static_cast<uint64_t>(std::max(kHeavyTaskMinSec * 1e9, kHeavyTaskShare * blockNs));
    int heavy = heavyNow_.exchange(0, std::memory_order_relaxed);
    if (heavy >= heavyEst_ || --heavyHold_ <= 0) heavyEst_ = heavy, heavyHold_ = kHeavyHoldBlocks;
    const bool worth = gate_.update(workNow_.exchange(0, std::memory_order_relaxed), heavy, blockNs);
    int waking = 0;
    for (const ProgChannel& ch : p->channels)
      if (ch.rt && ch.rt->resting && ch.rt->gen && ch.rt->gen->hasEvents()) ++waking;  // (a resting plugin's cost is not in the count)
    threads = std::min(threads, std::max(worth ? heavyEst_ : 1, waking));
  }
  if (threads <= 1 || n < kParallelMinFrames || g.active < 2) {
    serialBlocks_.fetch_add(1, std::memory_order_relaxed);
    if (offline_) {
      for (int t : g.serial) runTask(t, 0);
      return;
    }
    // K-142 (0.7.8): one after another, the end of one task is the start of the next (one clock
    // read per task instead of two: most tasks of an idle graph rest and take next to nothing).
    uint64_t at = nowNs();
    for (int t : g.serial) at = runTaskFrom(t, 0, at);
    return;
  }
  parallelBlocks_.fetch_add(1, std::memory_order_relaxed);
  for (int t : g.serial) g.pending[static_cast<size_t>(t)].store(g.deps[static_cast<size_t>(t)], std::memory_order_relaxed);
  int initial = 0;
  for (int t : g.serial)
    if (g.deps[static_cast<size_t>(t)] == 0) g.ready[static_cast<size_t>(initial++)].store(t, std::memory_order_relaxed);
  for (int k = initial; k < g.active; ++k) g.ready[static_cast<size_t>(k)].store(-1, std::memory_order_relaxed);
  readyTail_.store(initial, std::memory_order_relaxed);
  head_.store(0, std::memory_order_relaxed);
  done_.store(0, std::memory_order_relaxed);
  pool_.run(&AudioEngine::graphJob, this, std::min(threads - 1, g.active - 1));
}

void AudioEngine::graphJob(void* self, int worker) { static_cast<AudioEngine*>(self)->claimTasks(worker); }

void AudioEngine::claimTasks(int worker) {
  const ProgGraph& g = bc_.p->graph;
  const int total = g.active;
  int spins = 0;
  for (;;) {
    int h = head_.load(std::memory_order_acquire);
    if (h >= total) break;
    int t = g.ready[static_cast<size_t>(h)].load(std::memory_order_acquire);
    // Tracks that must not run on a worker thread wait at the head for the
    // audio thread (worker 0), which always takes part.
    if (t < 0 || (worker != 0 && !g.mainOnly.empty() && g.mainOnly[static_cast<size_t>(t)])) {  // nothing ready (for us) yet
      if (++spins > 64) {
        std::this_thread::yield();
        spins = 0;
      } else {
        cpuRelax();
      }
      continue;
    }
    if (!head_.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel)) continue;
    spins = 0;
    runTask(t, worker);
    for (int d : g.dependents[static_cast<size_t>(t)])
      if (g.pending[static_cast<size_t>(d)].fetch_sub(1, std::memory_order_acq_rel) == 1) {
        int k = readyTail_.fetch_add(1, std::memory_order_relaxed);
        g.ready[static_cast<size_t>(k)].store(d, std::memory_order_release);
      }
    done_.fetch_add(1, std::memory_order_release);
  }
  if (worker == 0)  // the audio thread returns only when every task has finished
    while (done_.load(std::memory_order_acquire) < total) cpuRelax();
}

void AudioEngine::runTask(int task, int worker) {
  if (offline_) {
    const tap::Bind tapBind(tapPass_);  // K-322: this engine's receipt pass on whichever thread runs the task (none: no-op)
    const int C = bc_.p->graph.channels;
    if (task < C)
      runChannel(static_cast<size_t>(task), worker);
    else
      runMixer(static_cast<size_t>(task - C), worker);
    return;
  }
  runTaskFrom(task, worker, nowNs());
}

// K-277: a realtime task timed from `t0`; returns the time it ended.
uint64_t AudioEngine::runTaskFrom(int task, int worker, uint64_t t0) {
  {
    const tap::Bind tapBind(tapPass_);  // K-322
    const int C = bc_.p->graph.channels;
    if (task < C)
      runChannel(static_cast<size_t>(task), worker);
    else
      runMixer(static_cast<size_t>(task - C), worker);
  }
  const uint64_t t1 = nowNs();
  const uint64_t d = t1 - t0;
  workNow_.fetch_add(d, std::memory_order_relaxed);
  if (d > heavyNs_) heavyNow_.fetch_add(1, std::memory_order_relaxed);
  return t1;
}

// K-87: a frozen unit's render at the song position: frame k is song sample k (the render starts
// at the song start, its plugin latency already taken out; a frozen track plays it `delay` frames late,
// as late as its effects were, and is compensated for that). Another rate is read with linear steps.
static void addFrozen(const Sample& s, const TempoMap& map, double sr, double anchorTick, int64_t clock, double delay, int from, int len,
                      float* L, float* R) {
  const double base = map.samplesBetween(0.0, anchorTick, sr) + static_cast<double>(clock) - delay;
  const int64_t frames = static_cast<int64_t>(s.frames());
  if (frames <= 0 || len <= 0) return;
  const float* sl = s.left.data();
  const float* sr2 = s.right.data();
  if (std::fabs(s.sampleRate - sr) < 1e-6) {
    const int64_t f0 = std::llround(base);
    const int64_t a = std::max<int64_t>(0, -f0), b = std::min<int64_t>(len, frames - f0);
    for (int64_t i = a; i < b; ++i) {
      L[from + i] += sl[f0 + i];
      R[from + i] += sr2[f0 + i];
    }
    return;
  }
  const double ratio = s.sampleRate / sr;
  for (int i = 0; i < len; ++i) {
    const double pos = (base + i) * ratio;
    if (pos < 0) continue;
    const auto k = static_cast<int64_t>(pos);
    if (k + 1 >= frames) break;
    const float t = static_cast<float>(pos - static_cast<double>(k));
    L[from + i] += sl[k] + (sl[k + 1] - sl[k]) * t;
    R[from + i] += sr2[k] + (sr2[k + 1] - sr2[k]) * t;
  }
}

void AudioEngine::runChannel(size_t ci, int worker) {
  const Program* p = bc_.p;
  const int n = bc_.n;
  const ProgChannel& ch = p->channels[ci];
  ChannelRuntime* rt = ch.rt.get();
  if (!rt || !rt->gen) return;
  tap::OwnerScope tapOwner(ch.id, bc_.ctx.tick, bc_.outFrame, bc_.ctx.sampleRate);  // J-219, K-321: Track Receipt passes (no-op otherwise)
  // K-317: the strip gains at each control block's start (a stepped block's own, else the one target).
  const bool stepped = bc_.steps > 1 && ch.stripLinked;
  auto target = [&](int j, float& l, float& r) {
    l = stepped ? ch.steps.l[j] : ch.gainL;
    r = stepped ? ch.steps.r[j] : ch.gainR;
  };
  const uint64_t t0 = bc_.profile ? nowNs() : 0;
  Scratch& sc = scratch_[static_cast<size_t>(worker)];
  float* cL = sc.chL;
  float* cR = sc.chR;
  // K-142 (0.7.8): a parameter that links or automation moved since the last block wakes a
  // resting instrument (it follows every change as it would have running).
  const bool moved = ch.liveMoved;
  ch.liveMoved = false;
  // K-64: a sampler channel with no voice, no event and no clip near this block makes silence:
  // nothing to run. (Most channels of a large arrangement, most of the time.)
  rt->outZero = false;
  rt->restWay = 0;  // K-142 (follow-up): set below when it rests
  const bool liveAudio = livePieceCount_ > 0 && liveOn();  // grid-1118: song mode under the clip grid too
  auto clipsNear = [&] {
    bool near = false;
    if (!ch.audio.empty() && tPlaying_ && tMode_ == PlayMode::Song)
      for (int sgi = 0; sgi < segCount_ && !near; ++sgi) near = audioClipsNear(ch.audio, segT0_[sgi], segTA_[sgi]);
    return near;
  };
  // K-277: Rest silent instruments. `calm`: nothing plays the instrument this block (no event, no
  // note held, no clip near, no transport jump, its editor closed) and its own smoothing has settled.
  // A muted strip (its gain at zero, not driven by a link) rests whatever plays; a MIDI message
  // for it (a controller, bend or program change routed from a MIDI Out channel) runs it for that
  // block, inaudibly, so it keeps that state for when it is heard again.
  const bool mayRest = bc_.rest && bc_.restInstruments && ch.restSilent;
  bool calm = false, mutedStrip = false;
  if (mayRest) {
    const Generator& g = *rt->gen;
    // K-142 (0.7.8): voices start only in a block the instrument runs: resting with no event
    // waiting, it has none (it began to rest with none), so their walk through every voice is left out.
    const bool voiceless = rt->resting && !rt->mutedRest && !g.hasEvents();
    calm = !g.hasEvents() && rt->pendingCount == 0 && rt->liveCount == 0 && !g.notesHeld() && !g.ownPlayback() &&
           !g.wantsNoteLookahead() && !(g.tailKnown() && !voiceless && g.active()) && g.smoothersSettled() &&
           !rt->editorOpen.load(std::memory_order_relaxed) && !bc_.ctx.jumped && !moved && !liveAudio && !clipsNear();
    mutedStrip = !ch.stripLinked && ch.gainL == 0.f && ch.gainR == 0.f && rt->primed && rt->curL == 0.f && rt->curR == 0.f &&
                 !rt->editorOpen.load(std::memory_order_relaxed) && !g.ownPlayback() && !(rt->mutedRest && g.hasMidi());
    if (rt->resting && !(rt->mutedRest ? mutedStrip : calm)) {  // woken: this block runs (its events at their offsets)
      rt->resting = rt->mutedRest = false;
      rt->quietFrames = 0;
      rt->faintQuiet = false;
    }
  } else if (rt->resting) {
    rt->resting = rt->mutedRest = false;
    rt->quietFrames = 0;
    rt->faintQuiet = false;
  }
  if (mayRest && rt->resting) {  // at rest: the block is silence, nothing runs
    if (rt->mutedRest) rt->gen->dropEvents();
    rt->gen->rested(n);
    rt->sounding.store(false, std::memory_order_relaxed);
    rt->restBlocks.fetch_add(1, std::memory_order_relaxed);
    rt->restingUi.store(true, std::memory_order_relaxed);
    idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
    rt->outZero = true;
    rt->peak.store(0.f, std::memory_order_relaxed);
    rt->tap.writeSilence(n);  // K-142
    rt->restWay = rt->mutedRest ? 0 : 1;  // K-142 (follow-up)
    if (bc_.profile) rt->cpu.add(nowNs() - t0);
    return;
  }
  if (mayRest) {
    rt->runBlocks.fetch_add(1, std::memory_order_relaxed);
    rt->restingUi.store(false, std::memory_order_relaxed);
  }
  // K-142 (0.7.8): left out the block before with no event since: still idle (its voices start only
  // when it runs), so the walk through its voices is left out.
  const bool stillIdle = rt->idleHeld && !rt->gen->hasEvents();
  rt->idleHeld = false;
  if (bc_.rest && ch.restable && (stillIdle || rt->gen->idle()) && !rt->gen->wantsNoteLookahead() && !liveAudio) {
    if (!clipsNear()) {
      rt->idleHeld = true;
      rt->sounding.store(false, std::memory_order_relaxed);
      idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
      rt->outZero = true;
      rt->peak.store(0.f, std::memory_order_relaxed);
      rt->tap.writeSilence(n);  // K-142
      rt->restWay = 2;  // K-142 (follow-up)
      if (bc_.profile) rt->cpu.add(nowNs() - t0);
      return;
    }
  }
  std::memset(cL, 0, sizeof(float) * static_cast<size_t>(n));  // (K-142: only when the instrument runs)
  std::memset(cR, 0, sizeof(float) * static_cast<size_t>(n));
  if (ch.midiFirst) {
    // A MIDI Out channel with listeners ran before the graph (runMidiOut); it makes no sound.
  } else if (tPlaying_ && rt->gen->wantsLanes()) {
    // J-159: the lanes on this instrument's own parameters (it evaluates them itself)
    ParamLane lanes[kMaxParamLanes];
    int nl = 0;
    auto add = [&](const std::vector<ProgAutoLane>& v, double origin) {
      for (const ProgAutoLane& a : v)
        if (a.target.kind == ProgLink::ChanPlugin && a.target.chan == static_cast<int>(ci) && nl < kMaxParamLanes)
          lanes[nl++] = ParamLane{a.target.param, &a.lane, origin};
    };
    if (tMode_ == PlayMode::Song) {
      add(p->autos, 0);
    } else if (tMode_ == PlayMode::Pattern) {
      if (const ProgPattern* pat = p->pattern(tPattern_)) add(pat->autos, 0);
    }
    if (liveOn()) {  // grid-1118: under the clip grid in song mode too
      for (const ProgLiveTrack& lt : p->liveTracks) {
        const LiveTrack& st = liveTracks_[lt.track];
        if (st.active == kNoId) continue;
        const int li = p->liveClipIndex(st.active);
        if (li >= 0) add(p->liveClips[static_cast<size_t>(li)].autos, static_cast<double>(st.origin));
      }
    }
    ProcessContext ctx = bc_.ctx;
    ctx.lanes = lanes;
    ctx.laneCount = nl;
    rt->gen->setExpression(ch.expr);  // K-144
    rt->configuredIn.store(p);  // K-288: before the instrument takes this Program's aux
    if (bc_.count) rt->dspCalls.fetch_add(1, std::memory_order_relaxed);  // K-142
    rt->gen->process(ch.modded ? ch.live : ch.plugin, ch.aux, ctx, cL, cR, n);
  } else {
    rt->gen->setExpression(ch.expr);  // K-144
    rt->configuredIn.store(p);  // K-288
    if (bc_.count) rt->dspCalls.fetch_add(1, std::memory_order_relaxed);  // K-142
    rt->gen->process(ch.modded ? ch.live : ch.plugin, ch.aux, bc_.ctx, cL, cR, n);
  }
  rt->sounding.store(rt->gen->active(), std::memory_order_relaxed);
  if (livePieceCount_ > 0 && liveOn()) {  // live mode: launched audio clips on this channel
    for (int k = 0; k < livePieceCount_; ++k) {
      const LivePiece& lp = livePieces_[k];
      const ProgLiveClip& lc = p->liveClips[static_cast<size_t>(lp.clip)];
      if (lc.audioChannel == static_cast<int>(ci))
        renderAudioClips(lc.audio, flatMap(p), sr_, lp.anchor, lp.clock, lp.from, lp.len, cL, cR, ch.pitchOffset);
    }
  }
  if (!ch.audio.empty() && tPlaying_ && tMode_ == PlayMode::Song)  // arrangement: audio clips on this channel
    for (int sgi = 0; sgi < segCount_; ++sgi)
      renderAudioClips(ch.audio, songMap(p), sr_, segs_[sgi].anchor, segs_[sgi].clock, segs_[sgi].from, segs_[sgi].len, cL, cR,
                       ch.pitchOffset, sessionOver_.load(std::memory_order_relaxed) ? arrOff_ : nullptr);  // grid-1118
  if (ch.frozen && tPlaying_ && tMode_ == PlayMode::Song)  // K-87: a frozen unit's render
    for (int sgi = 0; sgi < segCount_; ++sgi)
      addFrozen(*ch.frozen, songMap(p), sr_, segs_[sgi].anchor, segs_[sgi].clock, ch.frozenDelay, segs_[sgi].from, segs_[sgi].len, cL, cR);
  if (ch.controller) {
    int k = rt->gen->controlOutputs(rt->ctl, kMaxControllerOutputs);
    for (int q = 0; q < k; ++q) rt->ctlUi[q].store(rt->ctl[q], std::memory_order_relaxed);
  }
  if (!rt->primed) {
    target(0, rt->curL, rt->curR);
    rt->primed = true;
  }
  float pk = 0;
  float* oL = rt->outL.data();
  float* oR = rt->outR.data();
  const int gpos = bc_.gpos;
  if (bc_.grid) {  // ramp over each control block, positioned on the absolute grid
    for (int j = 0; j < std::max(1, bc_.steps); ++j) {
      const GainPiece pc = gainPiece(j, bc_.steps, gpos, n);
      if (pc.gpos == 0) {
        rt->fromL = rt->curL;
        rt->fromR = rt->curR;
      }
      float tL = 0, tR = 0;
      target(j, tL, tR);
      const float fL = rt->fromL, fR = rt->fromR;
      const float* xL = cL + pc.off;
      const float* xR = cR + pc.off;
      float* yL = oL + pc.off;
      float* yR = oR + pc.off;
      for (int i = 0; i < pc.len; ++i) {
        float k = std::min(1.f, static_cast<float>(pc.gpos + i + 1) / static_cast<float>(kControlBlock));  // (R08-01: a held block may run past one control block)
        float l = xL[i] * (fL + (tL - fL) * k), r = xR[i] * (fR + (tR - fR) * k);
        yL[i] = l;
        yR[i] = r;
        pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
      }
      float k = static_cast<float>(pc.gpos + pc.len) / static_cast<float>(kControlBlock);
      rt->curL = pc.gpos + pc.len >= kControlBlock ? tL : fL + (tL - fL) * k;
      rt->curR = pc.gpos + pc.len >= kControlBlock ? tR : fR + (tR - fR) * k;
    }
  } else {
    float dL = (ch.gainL - rt->curL) / static_cast<float>(n), dR = (ch.gainR - rt->curR) / static_cast<float>(n);
    float gL = rt->curL, gR = rt->curR;
    for (int i = 0; i < n; ++i) {
      gL += dL;
      gR += dR;
      float l = cL[i] * gL, r = cR[i] * gR;
      oL[i] = l;
      oR[i] = r;
      pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
    }
    rt->curL = ch.gainL;
    rt->curR = ch.gainR;
  }
  storeMax(rt->level, pk);
  rt->peak.store(pk, std::memory_order_relaxed);  // J-186
  rt->tap.write(cL, cR, n);
  if (mayRest) noteQuiet(*rt, mutedStrip ? 0.f : std::max(peakOf(cL, n), peakOf(cR, n)), calm || mutedStrip, mutedStrip, n);
  if (bc_.profile) rt->cpu.add(nowNs() - t0);
}

// K-277: after a block the instrument ran. Counts the frames in a row it stayed below the floor
// with nothing playing it; it rests once that lasted its hold time (longer when only the sound
// tells the tail has ended). Sound that comes out of silence with nothing to cause it marks an
// instrument that plays by itself: it never rests.
void AudioEngine::noteQuiet(ChannelRuntime& rt, float peak, bool calm, bool muted, int n) {
  const Generator& g = *rt.gen;
  // GitHub #66: an instrument whose tail only its sound tells is quiet at an inaudible level (its
  // own noise floor), over a longer hold when its sound was faint rather than silent.
  const bool listened = !g.tailKnown() && !muted;
  if (peak > (listened ? kRestFaint : kRestFloor)) {
    if (calm && static_cast<double>(rt.quietFrames) >= kRestSelfAfter * sr_) rt.selfSound = true;
    rt.quietFrames = 0;
    rt.faintQuiet = false;
    return;
  }
  if (!calm) rt.faintQuiet = false;
  else if (peak > kRestFloor) rt.faintQuiet = true;
  const double hold = (muted ? kRestHoldMuted : !listened ? kRestHoldKnown : rt.faintQuiet ? kRestHoldFaint : kRestHoldListened) * sr_;
  rt.quietFrames = calm ? rt.quietFrames + n : 0;
  if (!calm || (rt.selfSound && !muted) || static_cast<double>(rt.quietFrames) < hold) return;
  rt.resting = true;
  rt.mutedRest = muted;
  if (muted) rt.gen->kill();  // (inaudible: the strip is at zero) its voices end; it starts clean when heard again
}

void AudioEngine::noteEffectQuiet(EffectRuntime& rt, const float* L, const float* R, bool silentIn, bool may, int n) {
  if (!may || !silentIn) {
    rt.faintFrames = 0;
    return;
  }
  // K-142 (0.7.8): an effect whose output may stay faint for a while and still sound again by
  // itself (a delay's echoes) holds at least that long, and such a gap is not a sound of its own.
  const double gap = rt.fx ? static_cast<double>(rt.fx->faintGap()) : 0.0;
  if (std::max(peakOf(L, n), peakOf(R, n)) > kRestFaint) {
    if (static_cast<double>(rt.faintFrames) >= std::max(kRestSelfAfter * sr_, gap)) rt.selfSound = true;
    rt.faintFrames = 0;
    return;
  }
  rt.faintFrames += n;
  if (!rt.selfSound && static_cast<double>(rt.faintFrames) >= std::max(kRestHoldFaint * sr_, gap)) rt.resting = true;
}

void AudioEngine::pullInput(const ProgInput& in, float* accL, float* accR, depth::RoomEarly* early) const {
  const Program* p = bc_.p;
  const int n = bc_.n;
  switch (in.kind) {
    case ProgInput::Channel: {
      const ChannelRuntime* rt = p->channels[static_cast<size_t>(in.src)].rt.get();
      in.tap.run(rt->outZero ? nullptr : rt->outL.data(), rt->outZero ? nullptr : rt->outR.data(), 1.0f, accL, accR, n);
      break;
    }
    case ProgInput::ChannelAux: {  // multi-output instrument: unity gain, muted with the channel
      const ProgChannel& ch = p->channels[static_cast<size_t>(in.src)];
      const ChannelRuntime* rt = ch.rt.get();
      const float* aL = nullptr;
      const float* aR = nullptr;
      const bool live = rt && rt->gen && !(ch.stripLinked ? ch.modMuted : ch.muted) && in.index < rt->gen->auxOutputCount() &&
                        rt->gen->auxOutput(in.index, aL, aR) && aL && aR;
      in.tap.run(live ? aL : nullptr, live ? aR : nullptr, 1.0f, accL, accR, n);
      break;
    }
    case ProgInput::Send:
    case ProgInput::Sidechain: {
      const ProgMixer& src = p->mixer[static_cast<size_t>(in.src)];
      const bool live = !src.rt || src.rt->sent;
      const size_t k = static_cast<size_t>(in.index);
      const bool send = in.kind == ProgInput::Send;
      float level = send ? src.sends[k].second : src.sidechain[k].second;
      const bool room = send && k < src.sendRoom.size() && src.sendRoom[k];  // the Stage room send
      const ProgRoom& sroom = p->roomOf(src);
      if (room) level *= static_cast<float>(depth::roomScale(src.depth, src.focus, sroom.depthK, sroom.front));
      const auto& preFlags = send ? src.sendPre : src.sidechainPre;
      const bool pre = k < preFlags.size() && preFlags[k];  // pre-fader tap
      const float* sL = (pre ? preL_ : busL_)[static_cast<size_t>(in.src)].data();
      const float* sR = (pre ? preR_ : busR_)[static_cast<size_t>(in.src)].data();
      if (room && src.roomTap) {  // pre-delay that follows distance, reflections from the stage's walls
        depth::RoomTap& rt = *src.roomTap;
        const int dly = sroom.preDelay ? static_cast<int>(std::lround(depth::preDelaySec(src.depth) * sr_)) : 0;
        rt.run(live ? sL : nullptr, live ? sR : nullptr, n, dly);
        in.tap.run(rt.outL, rt.outR, level, accL, accR, n);
        if (early && sroom.earlyOn) {
          depth::EarlyTap taps[3];
          depth::earlyTaps(roomPan(src.modPan, src.roomW), src.depth, taps, sroom.halfW, sroom.depth, sroom.wall);
          rt.early(taps, sr_, level, early->L, early->R, n);
          early->any = true;
        }
        break;
      }
      // K-142 (0.7.8): a source at rest sends silence: as no input (the same zeros), so a compensation
      // line on the way knows it carries silence.
      const bool heard = live && busSilentNow_[static_cast<size_t>(in.src)] == 0;
      in.tap.run(heard ? sL : nullptr, heard ? sR : nullptr, level, accL, accR, n);
      break;
    }
    case ProgInput::Track: {  // J-45: a track's output at unity (side inputs)
      const ProgMixer& src = p->mixer[static_cast<size_t>(in.src)];
      const bool live = (!src.rt || src.rt->sent) && busSilentNow_[static_cast<size_t>(in.src)] == 0;  // (K-142: at rest, as no input)
      in.tap.run(live ? busL_[static_cast<size_t>(in.src)].data() : nullptr, live ? busR_[static_cast<size_t>(in.src)].data() : nullptr,
                 1.0f, accL, accR, n);
      break;
    }
    case ProgInput::SlotSend: {
      const EffectRuntime* rt = p->mixer[static_cast<size_t>(in.src)].slots[static_cast<size_t>(in.index)].rt.get();
      const bool live = rt && rt->sent;
      if (in.out >= 0) {
        const bool ok = live && !rt->multiL[in.out].empty();
        in.tap.run(ok ? rt->multiL[in.out].data() : nullptr, ok ? rt->multiR[in.out].data() : nullptr, 1.0f, accL, accR, n);
        break;
      }
      in.tap.run(live ? rt->sendL.data() : nullptr, live ? rt->sendR.data() : nullptr, 1.0f, accL, accR, n);
      break;
    }
  }
}

// K-142 (0.7.8): pulling `in` this block would add nothing and change nothing: its source puts out
// exact silence (a channel left out, a track at rest or not sent) and its compensation line (if
// any) holds only silence at a settled delay.
bool AudioEngine::inputSilent(const ProgInput& in) const {
  const Program* p = bc_.p;
  if (!in.tap.idle()) return false;
  switch (in.kind) {
    case ProgInput::Channel: {
      const ChannelRuntime* rt = p->channels[static_cast<size_t>(in.src)].rt.get();
      return rt && rt->outZero;
    }
    case ProgInput::ChannelAux: return false;
    case ProgInput::Send:
    case ProgInput::Sidechain: {
      const ProgMixer& src = p->mixer[static_cast<size_t>(in.src)];
      const size_t k = static_cast<size_t>(in.index);
      if (in.kind == ProgInput::Send && k < src.sendRoom.size() && src.sendRoom[k]) return false;  // the Stage room's own lines
      return (src.rt && !src.rt->sent) || busSilentNow_[static_cast<size_t>(in.src)] != 0;
    }
    case ProgInput::Track: {
      const ProgMixer& src = p->mixer[static_cast<size_t>(in.src)];
      return (src.rt && !src.rt->sent) || busSilentNow_[static_cast<size_t>(in.src)] != 0;
    }
    case ProgInput::SlotSend: {
      const EffectRuntime* rt = p->mixer[static_cast<size_t>(in.src)].slots[static_cast<size_t>(in.index)].rt.get();
      return !(rt && rt->sent);
    }
  }
  return false;
}

// K-142 (0.7.8): a track that rested through the block before (its strip settled on silence, every
// effect resting, its bus holding silence) and whose inputs bring nothing now stays at rest: the
// block would clear the silent bus, add silence, find it silent and leave every effect and the strip
// out. Tracks with an audio input, the Stage room, pre-fader sends, a mute or a bypass take the
// whole way (as does anything not at rest).
bool AudioEngine::trackHushed(const ProgMixer& m, size_t idx, int n) const {
  const MixerRuntime* rt = m.rt.get();
  if (!rt || rt->quiet < static_cast<int64_t>(sr_) || busZeroLen_[idx] < n || m.inFirst >= 0 || m.roomEarly || m.hasPreSend || m.disabled ||
      (m.muteLinked ? (m.soloSilent || m.muteMod) : m.silent))
    return false;
  for (const auto& in : m.inputs)
    if (!inputSilent(in)) return false;
  if (m.hasSidechain)
    for (const auto& in : m.scInputs)
      if (!inputSilent(in)) return false;
  const bool lowLat = m.armed && lowLatMon_.load(std::memory_order_relaxed) && recordOn_.load(std::memory_order_relaxed);
  for (const auto& slot : m.slots) {
    if (!slot.rt) continue;
    for (const auto& in : slot.auxIn)
      if (!in.tap.idle()) return false;
    for (const auto& in : slot.sideIn)
      if (!in.tap.idle()) return false;
    if (!slot.rt->fx || !slot.enabled || (lowLat && slot.latency > 0)) continue;  // bypassed
    if (slot.restFrames > 0 && slot.rt->quiet >= slot.restFrames) continue;    // K-64: at rest on silence
    const bool mayDoze = slot.hostedRest && bc_.restInstruments && !tPlaying_ && !slot.rt->editorOpen.load(std::memory_order_relaxed) &&
                         (slot.builtinRest || slot.rt->fx->restsWhenFaint());
    if (!(slot.rt->resting && mayDoze)) return false;  // GitHub #66: at rest, and staying so with silence in
  }
  return true;
}

// ---- K-142 (0.7.8 follow-up): the whole graph at rest ---------------------------------------------
//
// With everything at rest (the transport stopped, every instrument resting or idle, every track left
// out whole), a block still walked every channel and track to find that each stays at rest: each
// instrument's checks (its events, notes held, voices, smoothing, editor, clips), each track's inputs
// and effects, and a clock read per task. Most of what those checks look at changes only through the
// audio thread's own work (a command, a MIDI message, a note processor, a link or automation value, a
// new Program); what can change from elsewhere is looked at for every node (events queued, notes
// pending or held live, an open editor, the instrument's own playback). When none of it changed,
// every node stays at rest as it did, and sleepGraph does each one's rest bookkeeping (as runChannel
// and runMixer do on their rest paths) without the checks. The output is the same silence; realtime
// playback only (offline renders never rest). With per-object CPU timing on (the mixer or the Performance
// window shown), nodes left out this way add no time (they took next to none).

void AudioEngine::noteGraphRest(const Program* p) {
  restProg_ = nullptr;
  if (!bc_.rest || tapPass_ || !p) return;
  for (const ProgChannel& ch : p->channels)
    if (ch.rt && ch.rt->gen && ch.rt->restWay == 0) return;
  for (size_t idx = 0; idx < bc_.nb; ++idx)
    if (!p->mixer[idx].rt || !p->mixer[idx].rt->hushed) return;
  restProg_ = p;
  restInstrPrev_ = bc_.restInstruments;
  lowLatPrev_ = lowLatMon_.load(std::memory_order_relaxed) && recordOn_.load(std::memory_order_relaxed);
  linkChangesRest_ = linkChanges_;
}

bool AudioEngine::nodesStayAtRest(const Program* p, int n) const {
  if (restProg_ != p || !p || linkChanges_ != linkChangesRest_ || !p->midiOrder.empty()) return false;
  if (restInstrPrev_ != restInstruments_.load(std::memory_order_relaxed)) return false;
  if (lowLatPrev_ != (lowLatMon_.load(std::memory_order_relaxed) && recordOn_.load(std::memory_order_relaxed))) return false;
  for (const ProgChannel& ch : p->channels) {
    const ChannelRuntime* rt = ch.rt.get();
    if (!rt || !rt->gen) continue;
    if (ch.liveMoved || rt->pendingCount != 0 || rt->liveCount != 0 || rt->editorOpen.load(std::memory_order_relaxed)) return false;
    const Generator& g = *rt->gen;
    if (g.hasEvents()) return false;
    if (rt->restWay == 1) {
      if (!rt->resting || rt->mutedRest || !ch.restSilent || g.ownPlayback()) return false;
    } else if (rt->restWay == 2) {
      if (!rt->idleHeld || !ch.restable) return false;
    } else {
      return false;
    }
  }
  const size_t nb = std::min(p->mixer.size(), busL_.size());
  for (size_t idx = 0; idx < nb; ++idx) {
    const ProgMixer& m = p->mixer[idx];
    if (!m.rt || !m.rt->hushed || busZeroLen_[idx] < n) return false;
    for (const uint16_t si : m.filledSlots)
      if (m.slots[si].rt->editorOpen.load(std::memory_order_relaxed)) return false;
  }
  return true;
}

bool AudioEngine::graphSleeps(const Program* p, int n) const {
  if (offline_ || !bc_.rest || !graphSleep_.load(std::memory_order_relaxed) || tapPass_ || tPlaying_ || bc_.ctx.jumped || bc_.steps > 1 || livePieceCount_ > 0) return false;
  return nodesStayAtRest(p, n);
}

void AudioEngine::sleepGraph(const Program* p, int n) {
  serialBlocks_.fetch_add(1, std::memory_order_relaxed);
  sleptBlocks_.fetch_add(1, std::memory_order_relaxed);
  const bool mayRest = bc_.rest && bc_.restInstruments;
  for (const ProgChannel& ch : p->channels) {  // as runChannel's rest paths
    ChannelRuntime* rt = ch.rt.get();
    if (!rt || !rt->gen) continue;
    auto target = [&](int, float& l, float& r) {
      l = ch.gainL;
      r = ch.gainR;
    };
    if (rt->restWay == 1) {
      rt->gen->rested(n);
      rt->sounding.store(false, std::memory_order_relaxed);
      rt->restBlocks.fetch_add(1, std::memory_order_relaxed);
      rt->restingUi.store(true, std::memory_order_relaxed);
    } else {
      if (mayRest && ch.restSilent) {
        rt->runBlocks.fetch_add(1, std::memory_order_relaxed);
        rt->restingUi.store(false, std::memory_order_relaxed);
      }
      rt->sounding.store(false, std::memory_order_relaxed);
    }
    idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
    rt->outZero = true;
    rt->peak.store(0.f, std::memory_order_relaxed);
    rt->tap.writeSilence(n);
  }
  for (size_t idx = 0; idx < bc_.nb; ++idx) {  // as runMixer's hushed path
    const ProgMixer& m = p->mixer[idx];
    MixerRuntime* rt = m.rt.get();
    const bool lowLat = m.armed && lowLatPrev_;
    for (const uint16_t si : m.filledSlots) {
      const ProgSlot& slot = m.slots[si];
      slot.rt->sent = false;
      if (!slot.rt->fx || !slot.enabled || (lowLat && slot.latency > 0)) continue;
      if (!(slot.restFrames > 0 && slot.rt->quiet >= slot.restFrames)) slot.rt->restBlocks.fetch_add(1, std::memory_order_relaxed);
      slot.rt->fx->rested(slot.modded ? slot.live : slot.plugin, slot.aux, bc_.ctx, n);  // K-142 (0.7.8): its clocks move on
      slot.rt->inTap.writeSilence(n);
      slot.rt->tap.writeSilence(n);
    }
    auto target = [&](int, float& l, float& r) {
      l = m.gainL;
      r = m.gainR;
    };
    idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
    rt->tap.writeSilence(n);
    rt->sent = true;
    busSilentNow_[idx] = 1;
  }
  hushedTracks_.fetch_add(bc_.nb, std::memory_order_relaxed);
}

void AudioEngine::runMixer(size_t idx, int worker) {
  const Program* p = bc_.p;
  const int n = bc_.n;
  if (idx >= bc_.nb) return;
  const ProgMixer& m = p->mixer[idx];
  MixerRuntime* rt = m.rt.get();
  tap::OwnerScope tapOwner(m.id, bc_.ctx.tick, bc_.outFrame, bc_.ctx.sampleRate);  // J-219, K-321: Track Receipt passes (effects that play files)
  // K-317: the strip gains at each control block's start (a stepped block's own, else the one target).
  const bool stepped = bc_.steps > 1 && m.stripLinked;
  auto target = [&](int j, float& l, float& r) {
    l = stepped ? m.steps.l[j] : m.gainL;
    r = stepped ? m.steps.r[j] : m.gainR;
  };
  const uint64_t t0 = bc_.profile ? nowNs() : 0;
  uint64_t slotNs = 0;
  Scratch& scr = scratch_[worker];
  float* bL = busL_[idx].data();
  float* bR = busR_[idx].data();
  const size_t bytes = sizeof(float) * static_cast<size_t>(n);
  if (rt) rt->hushed = false;  // K-142 (follow-up): set below when it is left out
  if (bc_.rest && trackHushed(m, idx, n)) {  // K-142 (0.7.8): at rest, and nothing comes in: as below, without the work
    const bool lowLat = m.armed && lowLatMon_.load(std::memory_order_relaxed) && recordOn_.load(std::memory_order_relaxed);
    for (const auto& slot : m.slots) {
      if (!slot.rt) continue;
      slot.rt->sent = false;
      if (!slot.rt->fx || !slot.enabled || (lowLat && slot.latency > 0)) continue;
      if (!(slot.restFrames > 0 && slot.rt->quiet >= slot.restFrames)) slot.rt->restBlocks.fetch_add(1, std::memory_order_relaxed);
      slot.rt->fx->rested(slot.modded ? slot.live : slot.plugin, slot.aux, bc_.ctx, n);  // K-142 (0.7.8): its clocks move on
      slot.rt->inTap.writeSilence(n);
      slot.rt->tap.writeSilence(n);
    }
    idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
    rt->tap.writeSilence(n);
    rt->sent = true;
    rt->hushed = true;
    busSilentNow_[idx] = 1;
    hushedTracks_.fetch_add(1, std::memory_order_relaxed);
    if (bc_.profile) rt->cpu.add(nowNs() - t0);
    return;
  }
  busSilentNow_[idx] = 0;
  const int zeroBefore = busZeroLen_[idx];  // K-142: the bus's silence is looked at again below
  busZeroLen_[idx] = 0;
  std::memset(bL, 0, bytes);
  std::memset(bR, 0, bytes);
  depth::RoomEarly* early = m.roomEarly.get();  // the Stage room track: its senders' reflections
  if (early) {
    std::memset(early->L, 0, bytes);
    std::memset(early->R, 0, bytes);
    early->any = false;
  }
  for (const auto& in : m.inputs) pullInput(in, bL, bR, early);
  if (m.inFirst >= 0) {  // audio input (J-8): meter, and monitoring through the track
    const float* iL = inputL(m);
    const float* iR = inputR(m);
    if (rt) {
      float pl = 0, pr = 0;
      for (int i = 0; i < n; ++i) {
        pl = std::max(pl, std::fabs(iL[i]));
        pr = std::max(pr, std::fabs(iR[i]));
      }
      if (pl > rt->inPeakL.load(std::memory_order_relaxed)) rt->inPeakL.store(pl, std::memory_order_relaxed);
      if (pr > rt->inPeakR.load(std::memory_order_relaxed)) rt->inPeakR.store(pr, std::memory_order_relaxed);
    }
    if (hearsInput(m.monitor, m.armed, bc_.ctx.playing, curTick_))  // K-123
      for (int i = 0; i < n; ++i) {
        bL[i] += iL[i];
        bR[i] += iR[i];
      }
  }
  if (m.hasSidechain) {
    std::memset(scL_[idx].data(), 0, bytes);
    std::memset(scR_[idx].data(), 0, bytes);
    for (const auto& in : m.scInputs) pullInput(in, scL_[idx].data(), scR_[idx].data());
  }
  for (const auto& slot : m.slots)
    if (slot.rt) slot.rt->sent = false;
  if (m.muteLinked ? (m.soloSilent || m.muteMod) : m.silent) {
    std::memset(bL, 0, bytes);
    std::memset(bR, 0, bytes);
    busZeroLen_[idx] = std::max(zeroBefore, n);  // K-142
    // Bus returns keep their compensation lines running on silence.
    for (const auto& slot : m.slots) {
      for (const auto& in : slot.auxIn) in.tap.run(nullptr, nullptr, 0.f, scr.auxL, scr.auxR, n);
      for (const auto& in : slot.sideIn) in.tap.run(nullptr, nullptr, 0.f, scr.sideL, scr.sideR, n);
    }
    if (rt) {
      rt->curL = rt->curR = 0;
      rt->primed = true;
      rt->sent = false;
      if (bc_.profile) rt->cpu.add(nowNs() - t0);
    }
    return;
  }
  if (m.disabled) {  // bypassed track: the summed input passes through untouched
    for (const auto& slot : m.slots) {
      for (const auto& in : slot.auxIn) in.tap.run(nullptr, nullptr, 0.f, scr.auxL, scr.auxR, n);
      for (const auto& in : slot.sideIn) in.tap.run(nullptr, nullptr, 0.f, scr.sideL, scr.sideR, n);
    }
    if (m.hasPreSend) {
      std::memcpy(preL_[idx].data(), bL, bytes);
      std::memcpy(preR_[idx].data(), bR, bytes);
    }
    if (rt) {
      rt->primed = false;  // the fader ramps in from its target when re-enabled
      storeMax(rt->peakL, peakOf(bL, n));
      storeMax(rt->peakR, peakOf(bR, n));
      storeMax(rt->rmsL, std::sqrt(sumSq(bL, n) / static_cast<float>(n)));
      storeMax(rt->rmsR, std::sqrt(sumSq(bR, n) / static_cast<float>(n)));
      rt->tap.write(bL, bR, n);
      rt->sent = true;
      if (bc_.profile) rt->cpu.add(nowNs() - t0);
    }
    return;
  }
  // K-123 low-latency monitoring: while recording, an armed track skips its effects that add latency.
  const bool lowLat = m.armed && lowLatMon_.load(std::memory_order_relaxed) && recordOn_.load(std::memory_order_relaxed);
  // K-142 (0.7.8): whether the bus holds exact silence, found once and kept while the slots it
  // goes through rest (a slot that runs may change it); and the side input's, found once.
  int busZero = -1, sideZero = -1;
  auto busSilent = [&] {
    if (busZero < 0) busZero = allZero(bL, n) && allZero(bR, n) ? 1 : 0;
    return busZero == 1;
  };
  auto sideSilent = [&] {
    if (sideZero < 0) sideZero = !m.hasSidechain || (allZero(scL_[idx].data(), n) && allZero(scR_[idx].data(), n)) ? 1 : 0;
    return sideZero == 1;
  };
  for (const auto& slot : m.slots) {
    if (!slot.rt || !slot.rt->fx || !slot.enabled || (lowLat && slot.latency > 0)) {
      if (slot.rt) {  // keep a bypassed return's compensation lines running
        for (const auto& in : slot.auxIn) in.tap.run(nullptr, nullptr, 0.f, scr.auxL, scr.auxR, n);
        for (const auto& in : slot.sideIn) in.tap.run(nullptr, nullptr, 0.f, scr.sideL, scr.sideR, n);
      }
      continue;
    }
    const uint64_t s0 = bc_.profile ? nowNs() : 0;
    // K-64: an effect resting: exact silence in, its own silence out, nothing to run.
    const bool mayRest = bc_.rest && slot.restFrames > 0;
    const bool quietIn = mayRest && busSilent();
    if (quietIn && slot.rt->quiet >= slot.restFrames) {
      slot.rt->fx->rested(slot.modded ? slot.live : slot.plugin, slot.aux, bc_.ctx, n);  // K-142 (0.7.8)
      slot.rt->inTap.writeSilence(n);  // K-142
      slot.rt->tap.writeSilence(n);
      if (bc_.profile) {
        const uint64_t d = nowNs() - s0;
        slot.rt->cpu.add(d);
        slotNs += d;
      }
      continue;
    }
    // GitHub #66: a hosted effect fed silence with the transport stopped, whose output stayed
    // inaudible, rests; sound coming in, the transport starting or its editor in use wakes it.
    const bool mayDoze = slot.hostedRest && bc_.rest && bc_.restInstruments && !tPlaying_ &&
                         !slot.rt->editorOpen.load(std::memory_order_relaxed) && (slot.builtinRest || slot.rt->fx->restsWhenFaint());
    // K-142 (0.7.8): a built-in effect on a track with a side input hears that too: both silent.
    const bool silentIn = mayDoze && (mayRest ? quietIn : busSilent()) && (!slot.builtinRest || sideSilent());
    if (slot.rt->resting && !silentIn) {
      slot.rt->resting = false;
      slot.rt->faintFrames = 0;
    }
    if (slot.rt->resting) {  // at rest: its silence in stays silence out
      slot.rt->restBlocks.fetch_add(1, std::memory_order_relaxed);
      slot.rt->fx->rested(slot.modded ? slot.live : slot.plugin, slot.aux, bc_.ctx, n);  // K-142 (0.7.8)
      slot.rt->inTap.writeSilence(n);  // K-142
      slot.rt->tap.writeSilence(n);
      if (bc_.profile) {
        const uint64_t d = nowNs() - s0;
        slot.rt->cpu.add(d);
        slotNs += d;
      }
      continue;
    }
    busZero = -1;  // K-142: the slot runs: the bus is looked at again after it
    bool blend = slot.mix < 0.999f;
    if (blend) {
      std::memcpy(scr.dryL, bL, bytes);
      std::memcpy(scr.dryR, bR, bytes);
    }
    {
      SideIo io;
      if (m.hasSidechain) {
        io.scL = scL_[idx].data();
        io.scR = scR_[idx].data();
      }
      if (slot.sideOwn) {  // J-45: the slot reads one source track instead
        io.scL = io.scR = nullptr;
        if (!slot.sideIn.empty()) {
          std::memset(scr.sideL, 0, bytes);
          std::memset(scr.sideR, 0, bytes);
          for (const auto& in : slot.sideIn) pullInput(in, scr.sideL, scr.sideR);
          io.scL = scr.sideL;
          io.scR = scr.sideR;
        }
      }
      if ((slot.sendTo >= 0 || slot.busSend >= 0) && !slot.rt->sendL.empty()) {
        std::memset(slot.rt->sendL.data(), 0, bytes);
        std::memset(slot.rt->sendR.data(), 0, bytes);
        io.sendL = slot.rt->sendL.data();
        io.sendR = slot.rt->sendR.data();
        slot.rt->sent = true;
      }
      for (int k = 0; k < kMaxSendOutputs; ++k)
        if (slot.multiSend[k] >= 0 && !slot.rt->multiL[k].empty()) {
          std::memset(slot.rt->multiL[k].data(), 0, bytes);
          std::memset(slot.rt->multiR[k].data(), 0, bytes);
          io.multiL[k] = slot.rt->multiL[k].data();
          io.multiR[k] = slot.rt->multiR[k].data();
          slot.rt->sent = true;
        }
      if (slot.busRecv >= 0) {
        std::memset(scr.auxL, 0, bytes);
        std::memset(scr.auxR, 0, bytes);
        for (const auto& in : slot.auxIn) pullInput(in, scr.auxL, scr.auxR);
        io.recvL = scr.auxL;
        io.recvR = scr.auxR;
      }
      slot.rt->fx->setSideIo(io);
    }
    slot.rt->inTap.write(bL, bR, n);
    if (bc_.count) slot.rt->dspCalls.fetch_add(1, std::memory_order_relaxed);  // K-142
    slot.rt->fx->processAux(slot.modded ? slot.live : slot.plugin, slot.aux, bc_.ctx, bL, bR, n);
    if (slot.controller) {
      int k = slot.rt->fx->controlOutputs(slot.rt->ctl, kMaxControllerOutputs);
      for (int q = 0; q < k; ++q) slot.rt->ctlUi[q].store(slot.rt->ctl[q], std::memory_order_relaxed);
    }
    if (blend)
      for (int i = 0; i < n; ++i) {
        bL[i] = scr.dryL[i] + (bL[i] - scr.dryL[i]) * slot.mix;
        bR[i] = scr.dryR[i] + (bR[i] - scr.dryR[i]) * slot.mix;
      }
    if (mayRest) slot.rt->quiet = quietIn && allZero(bL, n) && allZero(bR, n) ? slot.rt->quiet + n : 0;
    noteEffectQuiet(*slot.rt, bL, bR, silentIn, mayDoze, n);
    slot.rt->meter.store(slot.rt->fx->meter(), std::memory_order_relaxed);
    slot.rt->input.store(slot.rt->fx->inputLevel(), std::memory_order_relaxed);
    {
      float mb[4];
      int k = slot.rt->fx->meters(mb, 4);
      for (int q = 0; q < k; ++q) slot.rt->bands[q].store(mb[q], std::memory_order_relaxed);
    }
    slot.rt->tap.write(bL, bR, n);
    if (bc_.profile) {
      const uint64_t d = nowNs() - s0;
      slot.rt->cpu.add(d);
      slotNs += d;
    }
  }
  if (early && early->any) {  // the Stage room: reflections join after the reverb (they are not reverberated twice)
    for (int i = 0; i < n; ++i) {
      bL[i] += early->L[i];
      bR[i] += early->R[i];
    }
    busZero = -1;
  }
  // K-64: silence through a settled strip stays silence (the strip EQ's memory is gone after a second).
  const bool stripQuiet = bc_.rest && rt && busSilent();
  if (stripQuiet && rt->quiet >= static_cast<int64_t>(sr_)) {
    if (m.hasPreSend) {
      std::memset(preL_[idx].data(), 0, bytes);
      std::memset(preR_[idx].data(), 0, bytes);
    }
    idleRamp(rt->curL, rt->curR, rt->fromL, rt->fromR, rt->primed, bc_.grid, bc_.steps, bc_.gpos, n, target);
    rt->tap.writeSilence(n);  // K-142
    rt->sent = true;
    busZeroLen_[idx] = std::max(zeroBefore, n);  // K-142: silence from its start (the frames after n are as they were)
    busSilentNow_[idx] = 1;
    if (bc_.profile) rt->cpu.add(nowNs() - t0 - slotNs);
    return;
  }
  // Strip: EQ, stereo separation, swap, polarity, volume/pan (smoothed).
  if (rt) rt->eq.process(m.eq, sr_, bL, bR, n);
  const float dep = m.depth, foc = m.focus;  // the Stage: air shelf and focus bell (nothing at all at 0)
  if (rt) {
    if (dep > 0 || foc != 0) rt->depthShelf.process(dep, foc, sr_, bL, bR, n);
    else rt->depthShelf.off();
  }
  if (rt && !rt->primed) {
    target(0, rt->curL, rt->curR);
    rt->curXL = stepped ? m.steps.xl[0] : m.xlr;
    rt->curXR = stepped ? m.steps.xr[0] : m.xrl;
    rt->primed = true;
  }
  const bool grid = bc_.grid;
  const int gpos = bc_.gpos;
  // Stage placement (Song::panLaw): the channels are placed by the cross gains, so the side gain only
  // widens (narrowing is the placement's own half width).
  const bool cross = p->panLaw == 2;
  const float sep = roomSep(m.sep, m.roomW);  // K-224: the room's width
  float width = 1.0f + (cross ? std::max(sep, 0.0f) : sep), sign = m.invert ? -1.f : 1.f;
  const bool ms = (cross ? sep > 0 : sep != 0) || dep > 0;
  if (dep > 0) width *= static_cast<float>(depth::narrow(dep));  // far sources narrow a little
  const bool gridRamp = grid && rt;
  float* preL = m.hasPreSend ? preL_[idx].data() : nullptr;
  float* preR = m.hasPreSend ? preR_[idx].data() : nullptr;
  // K-317: one piece per control block of a stepped block (each as a block of its own), else one.
  for (int j = 0; j < std::max(1, bc_.steps); ++j) {
    const GainPiece pc = gainPiece(j, bc_.steps, gpos, n);
    float tL = 0, tR = 0;
    target(j, tL, tR);
    const float txl = stepped ? m.steps.xl[j] : m.xlr, txr = stepped ? m.steps.xr[j] : m.xrl;
    float gL = rt ? rt->curL : tL, gR = rt ? rt->curR : tR;
    const float dL = (tL - gL) / static_cast<float>(pc.len), dR = (tR - gR) / static_cast<float>(pc.len);
    float xl = rt ? rt->curXL : txl, xr = rt ? rt->curXR : txr;
    const float dxl = (txl - xl) / static_cast<float>(pc.len), dxr = (txr - xr) / static_cast<float>(pc.len);
    if (gridRamp && pc.gpos == 0) {
      rt->fromL = rt->curL;
      rt->fromR = rt->curR;
    }
    for (int i = pc.off; i < pc.off + pc.len; ++i) {
      float l = bL[i], r = bR[i];
      if (ms) {
        float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
        l = mid + side;
        r = mid - side;
      }
      if (m.swap) std::swap(l, r);
      if (preL) {  // pre-fader sends: after effects, EQ and stereo processing, before volume / pan
        preL[i] = l * sign;
        preR[i] = r * sign;
      }
      if (gridRamp) {
        float k = std::min(1.f, static_cast<float>(pc.gpos + i - pc.off + 1) / static_cast<float>(kControlBlock));  // (R08-01: a held block may run past one control block)
        gL = rt->fromL + (tL - rt->fromL) * k;
        gR = rt->fromR + (tR - rt->fromR) * k;
      } else {
        gL += dL;
        gR += dR;
      }
      if (cross) {
        xl += dxl;
        xr += dxr;
        bL[i] = (l * gL + r * xr) * sign;
        bR[i] = (r * gR + l * xl) * sign;
      } else {
        bL[i] = l * gL * sign;
        bR[i] = r * gR * sign;
      }
    }
    if (rt) {
      rt->curXL = txl, rt->curXR = txr;
      if (gridRamp && pc.gpos + pc.len < kControlBlock) {
        rt->curL = gL;
        rt->curR = gR;
      } else {
        rt->curL = tL;
        rt->curR = tR;
      }
    }
  }
  if (rt) {
    storeMax(rt->peakL, peakOf(bL, n));
    storeMax(rt->peakR, peakOf(bR, n));
    storeMax(rt->rmsL, std::sqrt(sumSq(bL, n) / static_cast<float>(n)));
    storeMax(rt->rmsR, std::sqrt(sumSq(bR, n) / static_cast<float>(n)));
    rt->tap.write(bL, bR, n);
    rt->sent = true;
    if (bc_.rest) rt->quiet = stripQuiet && allZero(bL, n) && allZero(bR, n) ? rt->quiet + n : 0;
    if (bc_.profile) rt->cpu.add(nowNs() - t0 - slotNs);
  }
}

}  // namespace gl

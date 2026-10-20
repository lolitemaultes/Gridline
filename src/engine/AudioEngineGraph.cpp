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
inline uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

void AudioEngine::setThreads(int n) {
  n = std::clamp(n, 1, WorkerPool::kMaxThreads);
  pool_.ensureThreads(n - 1);
  threads_.store(n, std::memory_order_relaxed);
}

int AudioEngine::outputLatency() const { return current_ ? current_->outputLatency : 0; }

void AudioEngine::runGraph(const Program* p, int n) {
  const ProgGraph& g = p->graph;
  const int threads = std::min(threads_.load(std::memory_order_relaxed), pool_.threads() + 1);
  if (threads <= 1 || n < kParallelMinFrames || g.active < 2) {
    for (int t : g.serial) runTask(t, 0);
    return;
  }
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
  const int C = bc_.p->graph.channels;
  if (task < C)
    runChannel(static_cast<size_t>(task), worker);
  else
    runMixer(static_cast<size_t>(task - C), worker);
}

void AudioEngine::runChannel(size_t ci, int worker) {
  const Program* p = bc_.p;
  const int n = bc_.n;
  const ProgChannel& ch = p->channels[ci];
  ChannelRuntime* rt = ch.rt.get();
  if (!rt || !rt->gen) return;
  const uint64_t t0 = bc_.profile ? nowNs() : 0;
  Scratch& sc = scratch_[static_cast<size_t>(worker)];
  float* cL = sc.chL;
  float* cR = sc.chR;
  std::memset(cL, 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(cR, 0, sizeof(float) * static_cast<size_t>(n));
  rt->gen->process(ch.modded ? ch.live : ch.plugin, ch.aux, bc_.ctx, cL, cR, n);
  if (livePieceCount_ > 0 && tMode_ == PlayMode::Live) {  // live mode: launched audio clips on this channel
    for (int k = 0; k < livePieceCount_; ++k) {
      const LivePiece& lp = livePieces_[k];
      const ProgLiveClip& lc = p->liveClips[static_cast<size_t>(lp.clip)];
      if (lc.audioChannel == static_cast<int>(ci))
        renderAudioClips(lc.audio, p->flatMap, sr_, lp.anchor, lp.clock, lp.from, lp.len, cL, cR, ch.pitchOffset);
    }
  }
  if (!ch.audio.empty() && tPlaying_ && tMode_ == PlayMode::Song)  // arrangement: audio clips on this channel
    for (int sgi = 0; sgi < segCount_; ++sgi)
      renderAudioClips(ch.audio, p->tempoMap, sr_, segs_[sgi].anchor, segs_[sgi].clock, segs_[sgi].from, segs_[sgi].len, cL, cR,
                       ch.pitchOffset);
  if (ch.controller) {
    int k = rt->gen->controlOutputs(rt->ctl, kMaxControllerOutputs);
    for (int q = 0; q < k; ++q) rt->ctlUi[q].store(rt->ctl[q], std::memory_order_relaxed);
  }
  if (!rt->primed) {
    rt->curL = ch.gainL;
    rt->curR = ch.gainR;
    rt->primed = true;
  }
  float pk = 0;
  float* oL = rt->outL.data();
  float* oR = rt->outR.data();
  const int gpos = bc_.gpos;
  if (bc_.grid) {  // ramp over the control block, positioned on the absolute grid
    if (gpos == 0) {
      rt->fromL = rt->curL;
      rt->fromR = rt->curR;
    }
    const float fL = rt->fromL, fR = rt->fromR, tL = ch.gainL, tR = ch.gainR;
    for (int i = 0; i < n; ++i) {
      float k = static_cast<float>(gpos + i + 1) / static_cast<float>(kControlBlock);
      float l = cL[i] * (fL + (tL - fL) * k), r = cR[i] * (fR + (tR - fR) * k);
      oL[i] = l;
      oR[i] = r;
      pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
    }
    float k = static_cast<float>(gpos + n) / static_cast<float>(kControlBlock);
    rt->curL = gpos + n >= kControlBlock ? tL : fL + (tL - fL) * k;
    rt->curR = gpos + n >= kControlBlock ? tR : fR + (tR - fR) * k;
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
  rt->tap.write(cL, cR, n);
  if (bc_.profile) rt->cpu.add(nowNs() - t0);
}

void AudioEngine::pullInput(const ProgInput& in, float* accL, float* accR) const {
  const Program* p = bc_.p;
  const int n = bc_.n;
  switch (in.kind) {
    case ProgInput::Channel: {
      const ChannelRuntime* rt = p->channels[static_cast<size_t>(in.src)].rt.get();
      in.tap.run(rt->outL.data(), rt->outR.data(), 1.0f, accL, accR, n);
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
      const float level = send ? src.sends[k].second : src.sidechain[k].second;
      const auto& preFlags = send ? src.sendPre : src.sidechainPre;
      const bool pre = k < preFlags.size() && preFlags[k];  // pre-fader tap
      const float* sL = (pre ? preL_ : busL_)[static_cast<size_t>(in.src)].data();
      const float* sR = (pre ? preR_ : busR_)[static_cast<size_t>(in.src)].data();
      in.tap.run(live ? sL : nullptr, live ? sR : nullptr, level, accL, accR, n);
      break;
    }
    case ProgInput::Track: {  // J-45: a track's output at unity (side inputs)
      const ProgMixer& src = p->mixer[static_cast<size_t>(in.src)];
      const bool live = !src.rt || src.rt->sent;
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

void AudioEngine::runMixer(size_t idx, int worker) {
  const Program* p = bc_.p;
  const int n = bc_.n;
  if (idx >= bc_.nb) return;
  const ProgMixer& m = p->mixer[idx];
  MixerRuntime* rt = m.rt.get();
  const uint64_t t0 = bc_.profile ? nowNs() : 0;
  uint64_t slotNs = 0;
  Scratch& scr = scratch_[worker];
  float* bL = busL_[idx].data();
  float* bR = busR_[idx].data();
  const size_t bytes = sizeof(float) * static_cast<size_t>(n);
  std::memset(bL, 0, bytes);
  std::memset(bR, 0, bytes);
  for (const auto& in : m.inputs) pullInput(in, bL, bR);
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
    const auto mon = static_cast<InputMonitor>(m.monitor);
    if (mon == InputMonitor::On || (mon == InputMonitor::Auto && (m.armed || !bc_.ctx.playing)))
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
  for (const auto& slot : m.slots) {
    if (!slot.rt || !slot.rt->fx || !slot.enabled) {
      if (slot.rt) {  // keep a bypassed return's compensation lines running
        for (const auto& in : slot.auxIn) in.tap.run(nullptr, nullptr, 0.f, scr.auxL, scr.auxR, n);
        for (const auto& in : slot.sideIn) in.tap.run(nullptr, nullptr, 0.f, scr.sideL, scr.sideR, n);
      }
      continue;
    }
    const uint64_t s0 = bc_.profile ? nowNs() : 0;
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
  // Strip: EQ, stereo separation, swap, polarity, volume/pan (smoothed).
  if (rt) rt->eq.process(m.eq, sr_, bL, bR, n);
  if (rt && !rt->primed) {
    rt->curL = m.gainL;
    rt->curR = m.gainR;
    rt->primed = true;
  }
  const bool grid = bc_.grid;
  const int gpos = bc_.gpos;
  float gL = rt ? rt->curL : m.gainL, gR = rt ? rt->curR : m.gainR;
  float dL = (m.gainL - gL) / static_cast<float>(n), dR = (m.gainR - gR) / static_cast<float>(n);
  float width = 1.0f + m.sep, sign = m.invert ? -1.f : 1.f;
  const bool gridRamp = grid && rt;
  float* preL = m.hasPreSend ? preL_[idx].data() : nullptr;
  float* preR = m.hasPreSend ? preR_[idx].data() : nullptr;
  if (gridRamp && gpos == 0) {
    rt->fromL = rt->curL;
    rt->fromR = rt->curR;
  }
  for (int i = 0; i < n; ++i) {
    float l = bL[i], r = bR[i];
    if (m.sep != 0) {
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
      float k = static_cast<float>(gpos + i + 1) / static_cast<float>(kControlBlock);
      gL = rt->fromL + (m.gainL - rt->fromL) * k;
      gR = rt->fromR + (m.gainR - rt->fromR) * k;
    } else {
      gL += dL;
      gR += dR;
    }
    bL[i] = l * gL * sign;
    bR[i] = r * gR * sign;
  }
  if (rt) {
    if (gridRamp && gpos + n < kControlBlock) {
      rt->curL = gL;
      rt->curR = gR;
    } else {
      rt->curL = m.gainL;
      rt->curR = m.gainR;
    }
    storeMax(rt->peakL, peakOf(bL, n));
    storeMax(rt->peakR, peakOf(bR, n));
    storeMax(rt->rmsL, std::sqrt(sumSq(bL, n) / static_cast<float>(n)));
    storeMax(rt->rmsR, std::sqrt(sumSq(bR, n) / static_cast<float>(n)));
    rt->tap.write(bL, bR, n);
    rt->sent = true;
    if (bc_.profile) rt->cpu.add(nowNs() - t0 - slotNs);
  }
}

}  // namespace gl

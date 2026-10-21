#include "AudioEngine.h"
#include "Denormals.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "Dsp.h"
#include "Resample.h"
#include "../packs/edison/edison.h"

namespace gl {

namespace {
constexpr uint32_t kLiveNoteBit = 0x80000000u;

inline float peakOf(const float* x, int n) {
  float p = 0;
  for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}
inline void storeMax(std::atomic<float>& a, float v) {
  float cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}
}  // namespace

AudioEngine::AudioEngine(double sampleRate) : sr_(sampleRate), queue_(kQueue) {
  // Pre-allocate every bus the mixer can have, so the audio thread never allocates.
  size_t buses = static_cast<size_t>(kMaxMixerInserts) + 2;
  busL_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  busR_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  scL_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  scR_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  preL_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  preR_.assign(buses, std::vector<float>(kMaxBlock, 0.f));
  for (auto* v : {&tmpL_, &tmpR_}) v->assign(kMaxBlock, 0.f);
  scratch_.resize(WorkerPool::kMaxThreads);
  for (auto& r : bendRange_) r.store(2.0f);
  for (auto& m : macro_) m.store(-1.f);
  clockOut_.out = &syncRing_;
  inQueue_.assign(kInputQueueFrames * kMaxInputChannels, 0.f);
  inBuf_.assign(kMaxInputChannels, std::vector<float>(kMaxBlock, 0.f));
  silence_.assign(kMaxBlock, 0.f);
  tickHist_.assign(kTickHistory, -1.0);
  recTick_.assign(kMaxBlock, -1.0);
}

// ---- audio input & recording (J-8) -------------------------------------------

void AudioEngine::setInputChannels(int channels) {
  inChannels_.store(std::clamp(channels, 0, kMaxInputChannels));
  clearInput();
}

void AudioEngine::clearInput() {
  inTail_.store(inHead_.load(std::memory_order_acquire), std::memory_order_release);
}

void AudioEngine::pushInput(const float* x, int frames) {
  const int ch = inChannels_.load(std::memory_order_relaxed);
  size_t head = inHead_.load(std::memory_order_relaxed);
  const size_t tail = inTail_.load(std::memory_order_acquire);
  for (int i = 0; i < frames; ++i) {
    if (head - tail >= kInputQueueFrames) break;  // full: the newest frames are dropped
    float* dst = &inQueue_[(head % kInputQueueFrames) * kMaxInputChannels];
    for (int c = 0; c < ch; ++c) dst[c] = x[static_cast<size_t>(i) * static_cast<size_t>(ch) + static_cast<size_t>(c)];
    ++head;
  }
  inHead_.store(head, std::memory_order_release);
}

float AudioEngine::takeInputPeak(int channel) {
  if (channel < 0 || channel >= kMaxInputChannels) return 0.f;
  return inPeak_[channel].exchange(0.f, std::memory_order_relaxed);
}

void AudioEngine::setRecordLatency(int samples) {
  recLatency_.store(std::clamp(samples, 0, static_cast<int>(kTickHistory) - kMaxBlock - 1));
}

void AudioEngine::pushRecEvent(const RecEvent& e) {
  const size_t h = reHead_.load(std::memory_order_relaxed);
  if (h - reTail_.load(std::memory_order_acquire) >= kRecEvents) return;  // UI stalled: drop
  recEvents_[h % kRecEvents] = e;
  reHead_.store(h + 1, std::memory_order_release);
}

bool AudioEngine::popRecEvent(RecEvent& e) {
  const size_t t = reTail_.load(std::memory_order_relaxed);
  if (t == reHead_.load(std::memory_order_acquire)) return false;
  e = recEvents_[t % kRecEvents];
  reTail_.store(t + 1, std::memory_order_release);
  return true;
}

void AudioEngine::readDeviceInput(int n) {
  const int ch = inChannels_.load(std::memory_order_relaxed);
  const size_t head = inHead_.load(std::memory_order_acquire);
  size_t tail = inTail_.load(std::memory_order_relaxed);
  size_t avail = head - tail;
  const size_t prefill = static_cast<size_t>(inPrefill_.load(std::memory_order_relaxed));
  const size_t un = static_cast<size_t>(n);
  if (!inPrimed_ && avail >= un + prefill && avail > 0) inPrimed_ = true;
  // Keep the queue short: drop a backlog (e.g. after the stream was paused).
  const size_t maxBacklog = un + prefill + std::max<size_t>(2 * std::max(un, prefill), 2048);
  if (avail > maxBacklog) {
    tail = head - (un + prefill);
    avail = un + prefill;
  }
  size_t take = inPrimed_ ? std::min(avail, un) : 0;
  if (inPrimed_ && take < un) {  // underrun: refill the jitter buffer
    inPrimed_ = false;
    if (avail > 0 || head > 0) inUnderruns_.fetch_add(1, std::memory_order_relaxed);
  }
  for (int c = 0; c < kMaxInputChannels; ++c) {
    float* dst = inBuf_[static_cast<size_t>(c)].data();
    if (c >= ch) {
      std::memset(dst, 0, sizeof(float) * un);
      continue;
    }
    for (size_t i = 0; i < take; ++i) dst[i] = inQueue_[((tail + i) % kInputQueueFrames) * kMaxInputChannels + static_cast<size_t>(c)];
    for (size_t i = take; i < un; ++i) dst[i] = 0.f;
    storeMax(inPeak_[c], peakOf(dst, n));
  }
  inTail_.store(tail + take, std::memory_order_release);
}

const float* AudioEngine::inputL(const ProgMixer& m) const {
  const int ch = inChannels_.load(std::memory_order_relaxed);
  return m.inFirst >= 0 && m.inFirst < ch ? inBuf_[static_cast<size_t>(m.inFirst)].data() : silence_.data();
}

const float* AudioEngine::inputR(const ProgMixer& m) const {
  const int ch = inChannels_.load(std::memory_order_relaxed);
  if (m.inStereo && m.inFirst >= 0 && m.inFirst + 1 < ch) return inBuf_[static_cast<size_t>(m.inFirst + 1)].data();
  return inputL(m);
}

void AudioEngine::captureRecording(const Program* p, int n, bool post) {
  const double pa = punchA_.load(std::memory_order_relaxed), pb = punchB_.load(std::memory_order_relaxed);
  const bool punch = pb > pa;
  for (size_t mi = 0; mi < p->mixer.size(); ++mi) {
    const ProgMixer& m = p->mixer[mi];
    MixerRuntime* rt = m.rt.get();
    if (!rt) continue;
    // J-31: tracks recording their output are captured after the graph.
    const bool tapsOutput = m.armed && m.recPoint != static_cast<uint8_t>(RecordPoint::Input);
    if (tapsOutput != post && m.armed) continue;
    if (post && !m.armed) continue;
    packs::edison::CaptureRing* ring = rt->rec.get();
    if (!m.armed || !ring) {
      if (rt->recActive && ring) pushRecEvent({RecEvent::End, m.id, ring->written(), 0});
      rt->recActive = false;
      continue;
    }
    const bool preFader = m.recPoint == static_cast<uint8_t>(RecordPoint::PostEffects) && mi < preL_.size();
    const float* L = !post ? inputL(m) : preFader ? preL_[mi].data() : mi < busL_.size() ? busL_[mi].data() : silence_.data();
    const float* R = !post ? inputR(m) : preFader ? preR_[mi].data() : mi < busR_.size() ? busR_[mi].data() : silence_.data();
    int runFrom = rt->recActive ? 0 : -1;
    // Output taps hear the music late by the track's delay compensation, not the input latency.
    const size_t lat = static_cast<size_t>(std::max(0, m.latOut()));
    const size_t base = tickHistPos_ - static_cast<size_t>(n);
    for (int i = 0; i < n; ++i) {
      const double t = !post ? recTick_[static_cast<size_t>(i)] : tickHist_[(base + static_cast<size_t>(i) - lat) & (kTickHistory - 1)];
      const bool in = t >= 0 && (!punch || (t >= pa && t < pb));
      // A jump of the position (loop wrap, seek) starts a new take.
      const bool jump = in && rt->recActive && (t < rt->recLastTick - 1e-6 || t > rt->recLastTick + 4.0);
      if (rt->recActive && (!in || jump)) {
        if (runFrom >= 0 && i > runFrom) ring->write(L + runFrom, R + runFrom, i - runFrom, true);
        runFrom = -1;
        pushRecEvent({RecEvent::End, m.id, ring->written(), 0});
        rt->recActive = false;
      }
      if (in && !rt->recActive) {
        pushRecEvent({RecEvent::Start, m.id, ring->written(), t});
        rt->recActive = true;
        runFrom = i;
      }
      if (in) rt->recLastTick = t;
    }
    if (runFrom >= 0 && n > runFrom) ring->write(L + runFrom, R + runFrom, n - runFrom, true);
  }
}

void AudioEngine::playWithCountIn(double ticks) {
  Cmd c{Cmd::Play};
  c.tick2 = std::max(0.0, ticks);
  send(c);
  playing_ = true;
  if (ticks > 0) countingIn_ = true;
}

AudioEngine::~AudioEngine() = default;

void AudioEngine::setSampleRate(double sr) { sr_ = sr; }

void AudioEngine::setProgram(std::shared_ptr<const Program> p) {
  uint64_t stamp = blocks_.load() + 2;
  if (current_) retired_.push_back({current_, stamp});
  current_ = std::move(p);
  live_.store(current_.get());
  collectGarbage();
}

void AudioEngine::jumpTo(Tick tick, Tick grid) {
  Cmd c{Cmd::Jump};
  c.tick = static_cast<double>(std::max<Tick>(0, tick));
  c.tick2 = static_cast<double>(std::max<Tick>(0, grid));
  send(c);
  seekTick_.store(c.tick, std::memory_order_relaxed);  // J-49: a jump sets the song position marker
  if (!playing_.load() || grid <= 0)
    posTick_ = c.tick;
  else
    jumpPendingUi_.store(true);
}

int AudioEngine::snapToScaleMask(int pitch, int root, uint16_t mask) {
  if ((mask & 0xFFF) == 0) return pitch;
  auto in = [&](int p) { return p >= 0 && p <= 127 && (mask >> (((p - root) % 12 + 12) % 12) & 1u); };
  for (int d = 0; d <= 6; ++d) {
    if (in(pitch - d)) return pitch - d;
    if (in(pitch + d)) return pitch + d;
  }
  return pitch;
}

void AudioEngine::setLogger(std::shared_ptr<AudioLogger> logger) {
  if (loggerOwner_) retiredLoggers_.push_back({loggerOwner_, blocks_.load() + 2});
  loggerOwner_ = std::move(logger);
  logger_.store(loggerOwner_.get());
  collectGarbage();
}

void AudioEngine::logAudio(const Program* p, const float* mL, const float* mR, int n) {
  AudioLogger* lg = logger_.load(std::memory_order_acquire);
  if (!lg || n <= 0) return;
  if (lg->source() == AudioLogger::Source::Master) {
    lg->write(mL, mR, n);
    return;
  }
  // The inputs of the armed tracks, summed.
  const int k = std::min(n, kEngineMaxBlock);
  std::fill(logL_, logL_ + k, 0.0f);
  std::fill(logR_, logR_ + k, 0.0f);
  for (const ProgMixer& m : p->mixer) {
    if (!m.armed || m.inFirst < 0) continue;
    const float* iL = inputL(m);
    const float* iR = inputR(m);
    for (int i = 0; i < k; ++i) {
      logL_[i] += iL[i];
      logR_[i] += iR[i];
    }
  }
  lg->write(logL_, logR_, k);
}

void AudioEngine::collectGarbage() {
  uint64_t b = blocks_.load();
  retiredLoggers_.erase(std::remove_if(retiredLoggers_.begin(), retiredLoggers_.end(), [&](const auto& r) { return b >= r.second; }),
                        retiredLoggers_.end());
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [&](const auto& r) { return b >= r.second; }),
                 retired_.end());
  retiredSamples_.erase(std::remove_if(retiredSamples_.begin(), retiredSamples_.end(),
                                      [&](const auto& r) { return b >= r.second; }),
                       retiredSamples_.end());
  // If the audio thread is not running, nothing is in flight beyond the newest few.
  if (retired_.size() > 64) retired_.erase(retired_.begin(), retired_.end() - 64);
}

void AudioEngine::send(const Cmd& c) {
  size_t h = qHead_.load(std::memory_order_relaxed);
  if (h - qTail_.load(std::memory_order_acquire) >= kQueue) return;  // full: drop (UI retries naturally)
  queue_[h % kQueue] = c;
  qHead_.store(h + 1, std::memory_order_release);
}

void AudioEngine::setInterpolation(int quality) {
  interp_ = std::clamp(quality, 0, 7);
  if (interp_ >= 2) sincKernel(samplerSincPoints(interp_));
}
void AudioEngine::play() { send({Cmd::Play}); playing_ = true; }
double AudioEngine::stopTarget() const {
  if (returnToSeek_.load(std::memory_order_relaxed)) return seekTick_.load(std::memory_order_relaxed);
  return mode_.load() == PlayMode::Song && uiLoopB_ > uiLoopA_ ? uiLoopA_ : 0.0;
}
void AudioEngine::stop() {
  Cmd c{Cmd::Stop};
  c.tick = stopTarget();
  send(c);
  playing_ = false;
  posTick_ = c.tick;
}
void AudioEngine::stopToStart() {
  seekTick_.store(0, std::memory_order_relaxed);
  send({Cmd::Stop});
  playing_ = false;
  posTick_ = 0;
}
void AudioEngine::pause() { send({Cmd::Pause}); playing_ = false; }
void AudioEngine::togglePlay() {
  if (playing_.load()) stop(); else play();
}
void AudioEngine::setMode(PlayMode m) {
  Cmd c{Cmd::Mode};
  c.value = m == PlayMode::Song ? 1.f : m == PlayMode::Live ? 2.f : 0.f;
  send(c);
  if (mode_.load() != m) seekTick_.store(0, std::memory_order_relaxed);  // the new mode starts over at 0
  mode_ = m;
}
void AudioEngine::setPattern(ObjectId pattern) {
  Cmd c{Cmd::Pattern};
  c.id = pattern;
  send(c);
  pattern_ = pattern;
}
void AudioEngine::setPosition(Tick tick, bool seek) {
  Cmd c{Cmd::Position};
  c.tick = static_cast<double>(std::max<Tick>(0, tick));
  send(c);
  posTick_ = c.tick;
  if (seek) seekTick_.store(c.tick, std::memory_order_relaxed);
}
void AudioEngine::setLoopRange(Tick start, Tick end) {
  Cmd c{Cmd::LoopRange};
  c.tick = static_cast<double>(std::max<Tick>(0, start));
  c.tick2 = static_cast<double>(std::max<Tick>(0, end));
  send(c);
  uiLoopA_ = c.tick;
  uiLoopB_ = c.tick2;
}
void AudioEngine::setLooping(bool loop) {
  Cmd c{Cmd::Loop};
  c.value = loop ? 1.f : 0.f;
  send(c);
}
void AudioEngine::noteOn(ObjectId channel, int pitch, float velocity) {
  Cmd c{Cmd::NoteOn};
  c.id = channel;
  c.pitch = pitch;
  c.value = velocity;
  send(c);
}
void AudioEngine::noteOff(ObjectId channel, int pitch) {
  Cmd c{Cmd::NoteOff};
  c.id = channel;
  c.pitch = pitch;
  send(c);
}
void AudioEngine::panic() { send({Cmd::Panic}); }

void AudioEngine::launchClip(ObjectId clip) {
  Cmd c{Cmd::Launch};
  c.id = clip;
  send(c);
  playing_ = true;
}
void AudioEngine::releaseClip(ObjectId clip) {
  Cmd c{Cmd::Release};
  c.id = clip;
  send(c);
}
void AudioEngine::stopLiveTrack(int track) {
  Cmd c{Cmd::LiveStop};
  c.pitch = track;
  send(c);
}
void AudioEngine::stopAllLive() { send({Cmd::LiveStopAll}); }
ObjectId AudioEngine::liveActive(int track) const {
  return track >= 0 && track < kMaxPlaylistTracks ? liveActiveUi_[track].load(std::memory_order_relaxed) : kNoId;
}
ObjectId AudioEngine::liveQueued(int track) const {
  return track >= 0 && track < kMaxPlaylistTracks ? liveQueuedUi_[track].load(std::memory_order_relaxed) : kNoId;
}

void AudioEngine::midiEvent(uint8_t status, uint8_t d1, uint8_t d2) {
  size_t h = mHead_.load(std::memory_order_relaxed);
  if (h - mTail_.load(std::memory_order_acquire) >= kMidiQueue) return;  // full: drop
  mqueue_[h % kMidiQueue] = {status, d1, d2};
  mHead_.store(h + 1, std::memory_order_release);
}

void AudioEngine::drainMidi(const Program* p) {
  size_t t = mTail_.load(std::memory_order_relaxed), h = mHead_.load(std::memory_order_acquire);
  constexpr uint32_t kMidiBit = 0x40000000u;
  auto chan = [&](ObjectId ch) -> const ProgChannel* {
    if (!p) return nullptr;
    int ci = p->channelIndex(ch);
    if (ci < 0) return nullptr;
    const ProgChannel& c = p->channels[static_cast<size_t>(ci)];
    return c.rt && (c.rt->gen || !c.layer.empty()) ? &c : nullptr;
  };
  auto release = [&](int pitch) {
    if (const ProgChannel* c = chan(midiNoteCh_[pitch])) liveOff(p, *c, kMidiBit | static_cast<uint32_t>(pitch));
    midiNoteCh_[pitch] = kNoId;
    sustained_[pitch] = false;
    sostenutoHeld_[pitch] = false;
    sostenutoOff_[pitch] = false;
  };
  // Channel a MIDI channel's notes and controllers go to.
  auto routed = [&](int midiCh) {
    ObjectId r = midiRoute_[midiCh & 15].load(std::memory_order_relaxed);
    return r != kNoId ? r : midiTarget_.load();
  };
  // Built-in instruments follow pitch bend by gliding their MIDI notes
  // (hosted plugins read the raw message instead).
  auto bendable = [&](const ProgChannel* c) {
    if (!c || !c->rt->gen || !c->rt->gen->supportsGlide() || isHostedType(c->plugin.type)) return false;
    for (const auto& nf : c->noteFx)
      if (nf.rt) return false;
    return true;
  };
  while (t != h) {
    MidiMsg m = mqueue_[t % kMidiQueue];
    ++t;
    int kind = m.s & 0xF0;
    int mch = m.s & 0x0F;
    int pitch = m.a & 0x7F;
    if (kind == 0x90 || kind == 0x80) {  // J-31: snap incoming notes to the scale
      const uint32_t sn = midiSnap_.load(std::memory_order_relaxed);
      if (sn & 0xFFF) pitch = snapToScaleMask(pitch, static_cast<int>(sn >> 12), static_cast<uint16_t>(sn & 0xFFF));
    }
    if (kind == 0x90 && m.b > 0) {
      if (midiNoteCh_[pitch] != kNoId) release(pitch);  // retrigger
      ObjectId ch = routed(mch);
      if (const ProgChannel* c = chan(ch)) {
        float vel = m.b / 127.0f;
        if (soft_ && softOn_.load(std::memory_order_relaxed)) vel *= softFactor_.load(std::memory_order_relaxed);
        float bend = bendable(c) ? bend_[mch] : 0.0f;
        liveOn(p, *c, kMidiBit | static_cast<uint32_t>(pitch), static_cast<float>(pitch) + bend, vel, mch);
        midiNoteCh_[pitch] = ch;
        midiNoteSrc_[pitch] = static_cast<uint8_t>(mch);
      }
    } else if (kind == 0x80 || (kind == 0x90 && m.b == 0)) {
      if (midiNoteCh_[pitch] == kNoId) continue;
      if (sostenutoHeld_[pitch])
        sostenutoOff_[pitch] = true;
      else if (sustain_)
        sustained_[pitch] = true;
      else
        release(pitch);
    } else if (kind == 0xE0) {  // pitch bend
      const ProgChannel* c = chan(routed(mch));
      if (c && c->rt->gen) c->rt->gen->midi(m.s, m.a, m.b, 0);
      const int raw = ((m.b & 0x7F) << 7) | (m.a & 0x7F);
      bend_[mch] = static_cast<float>(raw - 8192) / 8192.0f * bendRange_[mch].load(std::memory_order_relaxed);
      for (int n = 0; n < 128; ++n) {
        if (midiNoteCh_[n] == kNoId || midiNoteSrc_[n] != mch) continue;
        const ProgChannel* nc = chan(midiNoteCh_[n]);
        if (bendable(nc)) nc->rt->gen->glide(kMidiBit | static_cast<uint32_t>(n), n + nc->pitchOffset + bend_[mch], 0.004, 0);
      }
    } else if (kind == 0xA0 || kind == 0xD0) {  // pressure: to the instrument
      if (const ProgChannel* c = chan(routed(mch)); c && c->rt->gen) c->rt->gen->midi(m.s, m.a, m.b, 0);
    } else if (kind == 0xB0) {
      if (const ProgChannel* c = chan(routed(mch)); c && c->rt->gen) c->rt->gen->midi(m.s, m.a, m.b, 0);
      if (m.a == 64) {  // sustain pedal
        sustain_ = m.b >= 64;
        if (!sustain_)
          for (int n = 0; n < 128; ++n)
            if (sustained_[n] && !sostenutoHeld_[n]) release(n);
      } else if (m.a == 66 && sostenutoOn_.load(std::memory_order_relaxed)) {  // sostenuto: holds the notes down now
        bool down = m.b >= 64;
        if (down && !sostenuto_) {
          for (int n = 0; n < 128; ++n) sostenutoHeld_[n] = midiNoteCh_[n] != kNoId && !sustained_[n];
        } else if (!down && sostenuto_) {
          for (int n = 0; n < 128; ++n) {
            if (!sostenutoHeld_[n]) continue;
            sostenutoHeld_[n] = false;
            if (sostenutoOff_[n]) {
              sostenutoOff_[n] = false;
              if (sustain_)
                sustained_[n] = true;
              else
                release(n);
            }
          }
        }
        sostenuto_ = down;
      } else if (m.a == 67) {  // soft pedal
        soft_ = m.b >= 64;
      } else if (m.a == 120 || m.a == 123) {  // all sound / all notes off
        for (int n = 0; n < 128; ++n)
          if (midiNoteCh_[n] != kNoId) release(n);
        sustain_ = sostenuto_ = soft_ = false;
      }
    }
  }
  mTail_.store(t, std::memory_order_release);
}

void AudioEngine::preview(SamplePtr sample) {
  if (previewOwner_) retiredSamples_.push_back({previewOwner_, blocks_.load() + 2});
  previewOwner_ = std::move(sample);
  Cmd c{Cmd::Preview};
  c.sample = previewOwner_.get();
  send(c);
  previewing_ = previewOwner_ != nullptr;
  collectGarbage();
}

// ---------------------------------------------------------------------------
// Audio thread

double AudioEngine::loopStart(const Program* p) const {
  if (tMode_ != PlayMode::Song || hostFollow_) return 0.0;
  if (loopB_ > loopA_) return loopA_;  // an explicit time selection wins over the song loop marker
  return markerActions(p) && p->songLoopStart > 0 ? p->songLoopStart : 0.0;
}

// J-85: marker actions on the audio thread, at the exact sample. Song mode
// only; off while a time selection loops (it takes precedence), while
// following a host transport, and in offline renders (an export plays the
// song once, start to end, like FL's).
bool AudioEngine::markerActions(const Program* p) const {
  return p && tMode_ == PlayMode::Song && !offline_ && !hostFollow_ && !(loopB_ > loopA_);
}

const ProgMarkerEvent* AudioEngine::nextMarkerEvent(const Program* p, double tick) const {
  for (const auto& e : p->markerEvents) {
    if (e.tick <= tick + 1e-9) continue;
    if (e.kind == ProgMarkerEvent::LoopBack && tick < e.from - 1e-6) continue;  // not inside its loop
    return &e;
  }
  return nullptr;
}

bool AudioEngine::fireMarker(const Program* p, const ProgMarkerEvent& e, int at) {
  switch (e.kind) {
    case ProgMarkerEvent::Skip:
    case ProgMarkerEvent::LoopBack:
      releaseAll(p, at);
      landAt(e.target);
      return true;
    case ProgMarkerEvent::Pause:
    case ProgMarkerEvent::Stop:
      releaseAll(p, at, true);
      pendJump_ = false;
      tPlaying_ = false;
      // Pause holds at the marker; Stop returns where the Stop button does.
      landAt(e.kind == ProgMarkerEvent::Pause ? e.tick : returnToSeek_.load(std::memory_order_relaxed) ? seekTick_.load(std::memory_order_relaxed) : 0.0);
      return false;
  }
  return true;
}

double AudioEngine::transportEnd(const Program* p) const {
  if (pastLoop_ && tMode_ == PlayMode::Song && loopB_ > loopA_ && !hostFollow_) return static_cast<double>(p->songLoopEnd);
  return loopEnd(p);
}

void AudioEngine::landAt(double tick) {
  anchorTick_ = tick;
  clock_ = 0;
  pastLoop_ = tMode_ == PlayMode::Song && loopB_ > loopA_ && tick >= loopB_;
}

double AudioEngine::loopEnd(const Program* p) const {
  if (tMode_ == PlayMode::Song) return loopB_ > loopA_ && !hostFollow_ ? loopB_ : static_cast<double>(p->songLoopEnd);
  if (tMode_ == PlayMode::Live) return 1e12;  // free-running transport
  const ProgPattern* pat = p->pattern(tPattern_);
  return static_cast<double>(pat ? pat->length : p->barTicks);
}

void AudioEngine::sendOn(const ProgChannel& ch, uint32_t id, float pitch, float velocity, int offset, int color,
                         const NoteProps* props) {
  if (!ch.rt || !ch.rt->gen) return;
  bool chain = false;
  for (const auto& nf : ch.noteFx) chain = chain || nf.rt != nullptr;
  if (!chain) {
    if (props)
      ch.rt->gen->noteOn(id, pitch + ch.pitchOffset, velocity, offset, *props);
    else
      ch.rt->gen->noteOn(id, pitch + ch.pitchOffset, velocity, offset);
    ch.rt->keys.on(id, pitch, velocity);  // J-77
    return;
  }
  NoteMsg m;
  m.kind = NoteMsg::On;
  m.offset = offset;
  m.id = id;
  m.pitch = pitch;
  m.velocity = velocity;
  m.color = static_cast<int16_t>(props ? props->color : color);
  if (props) {
    m.pan = props->pan;
    m.release = props->release;
    m.modX = props->modX;
    m.modY = props->modY;
  }
  ch.rt->inbox.push(m);
}

// MIDI hosted instruments sent during the previous block (their MIDI output
// port) plays on the channels listening on that port, one block later at
// the same offsets. Note ids are derived from the source channel, MIDI
// channel and key, so the matching note-off finds its note.
void AudioEngine::routePluginMidi(const Program* p, int n) {
  for (size_t ci = 0; ci < p->channels.size(); ++ci) {
    const ProgChannel& src = p->channels[ci];
    if (src.midiTargets.empty() || !src.rt || !src.rt->gen) continue;
    Generator::MidiOutEvent ev[256];
    const int count = src.rt->gen->takeMidiOut(ev, 256);
    for (int k = 0; k < count; ++k) {
      const uint8_t st = ev[k].s & 0xF0, mch = ev[k].s & 0x0F;
      const int off = std::clamp(ev[k].offset, 0, std::max(0, n - 1));
      const uint32_t id = 0xE0000000u | ((static_cast<uint32_t>(ci) & 0x1FFFu) << 11) | (static_cast<uint32_t>(mch) << 7) | (ev[k].a & 0x7Fu);
      for (int t : src.midiTargets) {
        if (t < 0 || static_cast<size_t>(t) >= p->channels.size()) continue;
        const ProgChannel& dst = p->channels[static_cast<size_t>(t)];
        if (!dst.rt || !dst.rt->gen) continue;
        if (st == 0x90 && ev[k].b > 0) {
          sendOff(dst, id, off);  // a retrigger ends the previous note of that key
          sendOn(dst, id, static_cast<float>(ev[k].a & 0x7F), static_cast<float>(ev[k].b & 0x7F) / 127.0f, off, mch);
        } else if (st == 0x80 || st == 0x90) {
          sendOff(dst, id, off);
        } else if (st == 0xB0 || st == 0xC0 || st == 0xD0 || st == 0xE0 || st == 0xA0) {
          dst.rt->gen->midi(ev[k].s, ev[k].a, ev[k].b, off);
        }
      }
    }
  }
}

// ---- Note dispatch: live notes through layers, sequenced notes with the
// channel note settings (cut groups, mono, porta, polyphony, slide notes).

void AudioEngine::liveOn(const Program* p, const ProgChannel& ch, uint32_t id, float pitch, float velocity, int color) {
  // J-31: a channel's key range limits the notes it plays (layer splits).
  auto inRange = [pitch](const ProgChannel& c) { return pitch > c.keyLo - 0.5f && pitch < c.keyHi + 0.5f; };
  if (inRange(ch)) sendOn(ch, id, pitch, velocity, 0, color);
  if (ch.layer.empty()) return;
  // Layer: the children play too (random layers pick one child per note).
  const size_t pick = ch.layerMode == 1 ? ((id * 2654435761u) >> 7) % ch.layer.size() : 0;
  for (size_t k = 0; k < ch.layer.size(); ++k) {
    if (ch.layerMode == 1 && k != pick) continue;
    const size_t ci = static_cast<size_t>(ch.layer[k]);
    if (ci < p->channels.size() && inRange(p->channels[ci])) sendOn(p->channels[ci], id, pitch, velocity, 0, color);
  }
}

void AudioEngine::liveOff(const Program* p, const ProgChannel& ch, uint32_t id) {
  sendOff(ch, id, 0);
  for (int k : ch.layer)
    if (static_cast<size_t>(k) < p->channels.size()) sendOff(p->channels[static_cast<size_t>(k)], id, 0);
}

void AudioEngine::cutPending(const ProgChannel& ch, int index, int offset) {
  ChannelRuntime* rt = ch.rt.get();
  if (!rt || index < 0 || index >= rt->pendingCount) return;
  const uint32_t id = rt->pending[index].id;
  bool chain = false;
  for (const auto& nf : ch.noteFx) chain = chain || nf.rt != nullptr;
  if (chain)
    sendOff(ch, id, offset);
  else if (rt->gen)
    rt->gen->cut(id, offset), rt->keys.off(id);
  rt->pending[index] = rt->pending[--rt->pendingCount];
}

void AudioEngine::startNote(const Program* p, const ProgChannel& ch, const NoteEv& e, int offset, int64_t at, double spt) {
  ChannelRuntime* rt = ch.rt.get();
  Generator* gen = rt->gen.get();
  bool chain = false;
  for (const auto& nf : ch.noteFx) chain = chain || nf.rt != nullptr;
  const bool glides = gen->supportsGlide() && !chain;
  // Slide note: no new note; the channel's sounding notes glide by the
  // interval from the last started note to the slide note, over its length.
  // (Instruments that cannot glide play it as an ordinary note.)
  if ((e.flags & kNoteSlide) && glides) {
    if (rt->pendingCount == 0 || rt->lastPitch < 0) return;
    const float shift = e.pitch - rt->lastPitch;
    const double secs = static_cast<double>(std::max<Tick>(1, e.off - e.on)) * spt / sr_;
    for (int i = 0; i < rt->pendingCount; ++i)
      gen->glide(rt->pending[i].id, rt->pending[i].pitch + shift + ch.pitchOffset, secs, offset);
    return;
  }
  if (rt->pendingCount >= kMaxPendingNotes) return;
  // Cut groups: this note cuts the notes of channels "cut by" its group that
  // started earlier (notes starting together, e.g. a chord, survive).
  if (ch.cutGroup > 0)
    for (const auto& other : p->channels) {
      if (other.cutBy != ch.cutGroup || !other.rt) continue;
      for (int i = 0; i < other.rt->pendingCount;)
        if (other.rt->pending[i].at < at)
          cutPending(other, i, offset);
        else
          ++i;
    }
  const bool porta = (ch.porta || (e.flags & kNotePorta)) && glides && rt->lastPitch >= 0;
  if (ch.mono && rt->pendingCount > 0) {
    if (porta) {  // legato: the sounding note glides to the new pitch and takes over its length
      int keep = 0;
      for (int i = 1; i < rt->pendingCount; ++i)
        if (rt->pending[i].at > rt->pending[keep].at) keep = i;
      ChannelRuntime::Pending kept = rt->pending[keep];
      for (int i = rt->pendingCount - 1; i >= 0; --i)
        if (i != keep) {
          gen->cut(rt->pending[i].id, offset);
          rt->keys.off(rt->pending[i].id);
        }
      gen->glide(kept.id, e.pitch + ch.pitchOffset, ch.portaTime, offset);
      rt->keys.retarget(kept.id, e.pitch);
      kept.off = e.off;
      kept.pitch = e.pitch;
      kept.at = at;
      rt->pending[0] = kept;
      rt->pendingCount = 1;
      rt->lastPitch = e.pitch;
      return;
    }
    while (rt->pendingCount > 0) cutPending(ch, rt->pendingCount - 1, offset);
  }
  if (ch.polyphony > 0)
    while (rt->pendingCount >= ch.polyphony) {  // voice limit: the oldest note is cut
      int old = 0;
      for (int i = 1; i < rt->pendingCount; ++i)
        if (rt->pending[i].at < rt->pending[old].at) old = i;
      cutPending(ch, old, offset);
    }
  // Ids come from the channel's own counter (J-30): instruments seed their
  // randomness with them, and a global counter numbered notes in a different
  // order for every block size. Range 1 .. 0x10000000 (below note processor ids).
  const uint32_t id = 1u + (((rt->noteSeq++ & 0x1FFFFu) << 11) | (static_cast<uint32_t>(&ch - p->channels.data()) & 0x7FFu));
  const NoteProps* props = e.props.neutral() ? nullptr : &e.props;
  if (porta && std::fabs(rt->lastPitch - e.pitch) > 1e-4f) {
    sendOn(ch, id, rt->lastPitch, e.velocity, offset, 0, props);
    gen->glide(id, e.pitch + ch.pitchOffset, ch.portaTime, offset);
    rt->keys.retarget(id, e.pitch);
  } else {
    sendOn(ch, id, e.pitch, e.velocity, offset, 0, props);
  }
  ChannelRuntime::Pending pd;
  pd.id = id;
  pd.off = e.off;
  pd.at = at;
  pd.pitch = e.pitch;
  rt->pending[rt->pendingCount++] = pd;
  rt->lastPitch = e.pitch;
}

void AudioEngine::sendOff(const ProgChannel& ch, uint32_t id, int offset) {
  if (!ch.rt || !ch.rt->gen) return;
  bool chain = false;
  for (const auto& nf : ch.noteFx) chain = chain || nf.rt != nullptr;
  if (!chain) {
    ch.rt->gen->noteOff(id, offset);
    ch.rt->keys.off(id);  // J-77
    return;
  }
  NoteMsg m;
  m.kind = NoteMsg::Off;
  m.offset = offset;
  m.id = id;
  ch.rt->inbox.push(m);
}

void AudioEngine::releaseAll(const Program* p, int offset, bool hard) {
  for (const auto& c : p->channels) {
    if (!c.rt) continue;
    if (c.rt->gen)
      for (int i = 0; i < c.rt->pendingCount; ++i) sendOff(c, c.rt->pending[i].id, offset);
    c.rt->pendingCount = 0;
    if (hard) c.rt->keys.clear();
    if (hard && !c.noteFx.empty() && c.rt->gen) {
      NoteMsg m;
      m.kind = NoteMsg::AllOff;
      m.offset = offset;
      c.rt->inbox.push(m);
    }
  }
}

void AudioEngine::drain(const Program* p) {
  size_t t = qTail_.load(std::memory_order_relaxed);
  size_t h = qHead_.load(std::memory_order_acquire);
  if (p) {  // a new tempo map (tempo edit, tempo automation): continue from the current position
    uint64_t sig = clockMap(p).signature();
    if (sig != lastMapSig_ && lastMapSig_ != 0 && clock_ != 0) {
      anchorTick_ = curTick_;
      clock_ = 0;
    }
    lastMapSig_ = sig;
  }
  auto here = [&]() { return p ? tickAt(p, anchorTick_, static_cast<double>(clock_)) : anchorTick_; };
  while (t != h) {
    const Cmd c = queue_[t % kQueue];
    ++t;
    switch (c.kind) {
      case Cmd::Play:
        if (!tPlaying_) {
          startTick_ = here();
          anchorTick_ = startTick_;
          clock_ = 0;
          tPlaying_ = true;
          countInLeft_ = 0;
          if (c.tick2 > 0 && p) {  // count-in: clicks while the transport waits
            countInTotal_ = std::max<int64_t>(1, static_cast<int64_t>(p->flatMap.samplesBetween(0, c.tick2, sr_)));
            countInBeat_ = std::max<int64_t>(1, static_cast<int64_t>(p->flatMap.samplesBetween(0, static_cast<double>(std::max<Tick>(1, p->beatTicks)), sr_)));
            countInLeft_ = countInTotal_;
          }
          countingIn_.store(countInLeft_ > 0);
        }
        break;
      case Cmd::Stop:
        pendJump_ = false;
        jumpPendingUi_.store(false);
        countInLeft_ = 0;
        countingIn_.store(false);
        if (p) releaseAll(p, 0, true);
        clearLive();
        tPlaying_ = false;
        landAt(c.tick);  // J-49: the seek position (0 by default); past a selection stays past it (J-40)
        break;
      case Cmd::Pause:
        countInLeft_ = 0;
        countingIn_.store(false);
        if (p) releaseAll(p, 0, true);
        anchorTick_ = here();
        clock_ = 0;
        tPlaying_ = false;
        break;
      case Cmd::Mode:
        if (p) releaseAll(p, 0);
        tMode_ = c.value > 1.5f ? PlayMode::Live : c.value > 0.5f ? PlayMode::Song : PlayMode::Pattern;
        clearLive();
        anchorTick_ = 0;
        clock_ = 0;
        pastLoop_ = false;
        break;
      case Cmd::Pattern:
        if (tPattern_ != c.id && p && tMode_ == PlayMode::Pattern) releaseAll(p, 0);
        tPattern_ = c.id;
        break;
      case Cmd::Position:
        if (p) releaseAll(p, 0);
        landAt(c.tick);
        pendJump_ = false;
        jumpPendingUi_.store(false);
        break;
      case Cmd::Jump:  // J-31: quantized song position jump
        if (!tPlaying_ || c.tick2 <= 0 || !p) {
          if (p) releaseAll(p, 0);
          landAt(c.tick);
          pendJump_ = false;
          jumpPendingUi_.store(false);
        } else {
          const double cur = here();
          pendJumpAt_ = std::ceil(cur / c.tick2 - 1e-9) * c.tick2;
          if (pendJumpAt_ < cur) pendJumpAt_ += c.tick2;
          pendJumpTo_ = c.tick;
          pendJump_ = true;
          jumpPendingUi_.store(true);
        }
        break;
      case Cmd::Loop: tLoop_ = c.value > 0.5f; break;
      case Cmd::LoopRange: {
        loopA_ = c.tick;
        loopB_ = c.tick2;
        const double at = here();
        pastLoop_ = tMode_ == PlayMode::Song && loopB_ > loopA_ && at >= loopB_ && pastLoop_;  // stays past a moved selection
        break;
      }
      case Cmd::Preview:
        pvSample_ = c.sample;
        pvPos_ = 0;
        break;
      case Cmd::NoteOn:
      case Cmd::NoteOff: {
        if (!p) break;
        int ci = p->channelIndex(c.id);
        if (ci < 0) break;
        const ProgChannel& ch = p->channels[static_cast<size_t>(ci)];
        if (!ch.rt || (!ch.rt->gen && ch.layer.empty())) break;
        uint32_t id = kLiveNoteBit | static_cast<uint32_t>(c.pitch & 0x7F);
        if (c.kind == Cmd::NoteOn)
          liveOn(p, ch, id, static_cast<float>(c.pitch), c.value, 0);
        else
          liveOff(p, ch, id);
        break;
      }
      case Cmd::Launch:
      case Cmd::Release:
      case Cmd::LiveStop:
      case Cmd::LiveStopAll:
        if (p) {
          if (c.kind == Cmd::Launch && !tPlaying_) {  // a launch starts the transport
            startTick_ = here();
            anchorTick_ = startTick_;
            clock_ = 0;
            tPlaying_ = true;
          }
          liveCommand(p, c);
        }
        break;
      case Cmd::Panic:
        if (p)
          for (const auto& ch : p->channels)
            if (ch.rt && ch.rt->gen) {
              ch.rt->gen->kill();
              ch.rt->keys.clear();
              ch.rt->pendingCount = 0;
              ch.rt->inbox.clear();
              for (const auto& nf : ch.noteFx)
                if (nf.rt) nf.rt->fx->reset();
            }
        break;
    }
  }
  qTail_.store(t, std::memory_order_release);
}

void AudioEngine::dispatch(const Program* p, int from, int to) {
  const int len = to - from;
  if (len <= 0) return;
  auto sampleOf = [&](double tick) { return sampleAt(p, anchorTick_, tick); };
  const int64_t c0 = clock_, c1 = clock_ + len;
  const double tLo = tickAt(p, anchorTick_, static_cast<double>(c0 - 1));
  const double tHi = tickAt(p, anchorTick_, static_cast<double>(c1)) + 1.0;
  const ProgPattern* pat = tMode_ == PlayMode::Pattern ? p->pattern(tPattern_) : nullptr;
  if (tMode_ == PlayMode::Live) dispatchLive(p, from, to);  // launched clips (note-offs below)

  // J-30: notes start in time order across all channels (then channel order),
  // and note-offs due before a start are sent before it. Handling channel by
  // channel made the result depend on where block boundaries fall (cut
  // groups, mono / polyphony limits saw notes that had already ended).
  int nStarts = 0;
  for (size_t ci = 0; ci < p->channels.size(); ++ci) {
    const ProgChannel& ch = p->channels[ci];
    ChannelRuntime* rt = ch.rt.get();
    if (!rt || !rt->gen) continue;
    const std::vector<NoteEv>* evs = nullptr;
    if (tMode_ == PlayMode::Live)
      evs = nullptr;
    else if (tMode_ == PlayMode::Song)
      evs = &ch.song;
    else if (pat && ci < pat->perChannel.size())
      evs = &pat->perChannel[ci];
    if (!evs || evs->empty()) continue;
    auto it = std::lower_bound(evs->begin(), evs->end(), static_cast<Tick>(std::floor(tLo)),
                               [](const NoteEv& e, Tick t) { return e.on < t; });
    for (; it != evs->end() && static_cast<double>(it->on) <= tHi; ++it) {
      int64_t s = sampleOf(static_cast<double>(it->on));
      if (s < c0 || s >= c1) continue;
      if (nStarts >= kMaxBlockStarts) break;
      starts_[static_cast<size_t>(nStarts++)] = {s, static_cast<int>(ci), &*it};
    }
  }
  std::sort(starts_.begin(), starts_.begin() + nStarts, [](const BlockStart& a, const BlockStart& b) {
    return a.at != b.at ? a.at < b.at : (a.ch != b.ch ? a.ch < b.ch : a.ev < b.ev);
  });
  // Note-offs due before clock sample `lim` (notes started just now included).
  auto offsBefore = [&](int64_t lim) {
    for (const ProgChannel& ch : p->channels) {
      ChannelRuntime* rt = ch.rt.get();
      if (!rt || !rt->gen) continue;
      for (int i = 0; i < rt->pendingCount;) {
        int64_t s = sampleOf(static_cast<double>(rt->pending[i].off));
        if (s < lim) {
          int off = s < c0 ? from : from + static_cast<int>(s - c0);
          sendOff(ch, rt->pending[i].id, off);
          rt->pending[i] = rt->pending[--rt->pendingCount];
        } else {
          ++i;
        }
      }
    }
  };
  for (int k = 0; k < nStarts; ++k) {
    const BlockStart& b = starts_[static_cast<size_t>(k)];
    if (k == 0 || starts_[static_cast<size_t>(k - 1)].at != b.at) offsBefore(b.at);
    const ProgChannel& ch = p->channels[static_cast<size_t>(b.ch)];
    if (ch.rt->pendingCount >= kMaxPendingNotes) continue;
    startNote(p, ch, *b.ev, from + static_cast<int>(b.at - c0), absClock_ + from + (b.at - c0),
              sr_ * 60.0 / (clockMap(p).bpmAt(static_cast<double>(b.ev->on)) * p->ppq));  // local tempo (tempo map)
  }
  offsBefore(c1);
}

void AudioEngine::processBlock(const Program* p, float* L, float* R, int n) {
  std::memset(L, 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(R, 0, sizeof(float) * static_cast<size_t>(n));
  readDeviceInput(n);
  if (!p) return;

  // --- transport & sequencing ---
  const double blockTick = tickAt(p, anchorTick_, static_cast<double>(clock_));
  segCount_ = 0;
  livePieceCount_ = 0;
  // MIDI sync output: mode changes, stops.
  const SyncOut syncMode = static_cast<SyncOut>(syncMode_.load(std::memory_order_relaxed));
  if (syncMode != clockOut_.mode()) {
    clockOut_.stop(absClock_);
    clockOut_.setMode(syncMode);
  }
  clockOut_.setPpq(p->ppq);
  clockOut_.setSampleRate(sr_);
  if (!tPlaying_ && clockOut_.running()) clockOut_.stop(absClock_);
  if (tPlaying_) {
    int s = 0, guard = 0;
    if (countInLeft_ > 0) {  // count-in: clicks on the beats, the transport waits
      const int k = static_cast<int>(std::min<int64_t>(n, countInLeft_));
      const int64_t e0 = countInTotal_ - countInLeft_;
      const int64_t b = (e0 + countInBeat_ - 1) / countInBeat_;
      const int64_t at = b * countInBeat_ - e0;
      if (at >= 0 && at < k) {
        const int64_t beatsPerBar = std::max<Tick>(1, p->barTicks / std::max<Tick>(1, p->beatTicks));
        clickStart_ = static_cast<int>(at);
        clickLeft_ = static_cast<int>(sr_ * 0.03);
        clickPhase_ = 0;
        clickFreq_ = b % beatsPerBar == 0 ? 1600.0 : 1000.0;
      }
      countInLeft_ -= k;
      s = k;
      if (countInLeft_ <= 0) countingIn_.store(false);
    }
    while (s < n && guard++ < 64) {
      const double end = transportEnd(p);
      int seg = n - s;
      bool wrap = false, jump = false;
      if (tLoop_) {
        int64_t endSample = sampleAt(p, anchorTick_, end);
        if (endSample <= clock_) {  // already past the end (e.g. a shorter pattern was selected)
          releaseAll(p, s);
          landAt(pendJump_ ? pendJumpTo_ : loopStart(p));
          pendJump_ = false;
          continue;
        }
        if (endSample < clock_ + seg) {
          seg = static_cast<int>(endSample - clock_);
          wrap = true;
        }
      }
      if (pendJump_) {  // J-31: a quantized jump lands on its grid line
        const int64_t js = sampleAt(p, anchorTick_, pendJumpAt_);
        if (js <= clock_) {
          releaseAll(p, s);
          landAt(pendJumpTo_);
          pendJump_ = false;
          continue;
        }
        if (js < clock_ + seg) {
          seg = static_cast<int>(js - clock_);
          wrap = false;
          jump = true;
        }
      }
      // J-85: a marker action inside this stretch ends it on its sample.
      const ProgMarkerEvent* mev = nullptr;
      if (markerActions(p)) {
        mev = nextMarkerEvent(p, tickAt(p, anchorTick_, static_cast<double>(clock_)));
        if (mev) {
          const int64_t ms = sampleAt(p, anchorTick_, mev->tick);
          if (ms <= clock_) {
            if (!fireMarker(p, *mev, std::min(s, n - 1))) break;
            continue;
          }
          if (ms <= clock_ + seg) {  // on this stretch's last edge too (the next block would start past it)
            seg = static_cast<int>(ms - clock_);
            wrap = jump = false;
          } else {
            mev = nullptr;
          }
        }
      }
      if (segCount_ < kMaxSegments) segs_[segCount_++] = {s, seg, anchorTick_, clock_};
      if (syncMode != SyncOut::Off) {  // clock / MTC for this stretch, at exact sample positions
        const TempoMap& map = clockMap(p);
        const double t0 = tickAt(p, anchorTick_, static_cast<double>(clock_));
        const double t1 = tickAt(p, anchorTick_, static_cast<double>(clock_ + seg));
        if (!clockOut_.running()) clockOut_.start(t0, map.secondsAt(t0), absClock_ + s);
        const double anchor = anchorTick_;
        const int64_t c0 = clock_;
        clockOut_.segment(t0, t1, map.secondsAt(t0), map.secondsAt(t1), absClock_ + s, seg, [&](double tk) {
          return map.samplesBetween(anchor, tk, sr_) - static_cast<double>(c0);
        });
      }
      dispatch(p, s, s + seg);
      if (metronome_.load(std::memory_order_relaxed)) {  // click on each beat in this segment
        const Tick beatT = std::max<Tick>(1, p->beatTicks);
        double t0 = tickAt(p, anchorTick_, static_cast<double>(clock_));
        double t1 = tickAt(p, anchorTick_, static_cast<double>(clock_ + seg));
        Tick first = static_cast<Tick>(std::ceil(t0 - 1e-9));
        Tick b0 = (first + beatT - 1) / beatT * beatT;
        if (b0 < t1) {
          int64_t smp = sampleAt(p, anchorTick_, static_cast<double>(b0)) - clock_;
          if (smp >= 0 && smp < seg) {
            clickStart_ = s + static_cast<int>(smp);
            clickLeft_ = static_cast<int>(sr_ * 0.03);
            clickPhase_ = 0;
            clickFreq_ = (b0 % std::max<Tick>(1, p->barTicks)) == 0 ? 1600.0 : 1000.0;
          }
        }
      }
      clock_ += seg;
      s += seg;
      if (mev) {
        if (!fireMarker(p, *mev, std::min(s, n - 1))) break;
        continue;
      }
      if (wrap || jump) {
        releaseAll(p, std::min(s, n - 1));
        landAt(pendJump_ ? pendJumpTo_ : loopStart(p));  // a pending jump replaces the loop wrap
        pendJump_ = false;
      }
    }
    if (!pendJump_) jumpPendingUi_.store(false, std::memory_order_relaxed);
    curTick_ = tickAt(p, anchorTick_, static_cast<double>(clock_));
    posTick_.store(curTick_);
  }
  playing_.store(tPlaying_);
  // --- recording: the position of every frame, delayed by the input latency
  // (the music the performer heard when the input frame was played) ---
  const bool recOn = recordOn_.load(std::memory_order_relaxed);
  {
    double* cur = recTick_.data();
    for (int i = 0; i < n; ++i) cur[i] = -1.0;
    if (tPlaying_ && recOn)
      for (int k = 0; k < segCount_; ++k) {
        const Segment& sg = segs_[k];
        const double t0 = tickAt(p, sg.anchor, static_cast<double>(sg.clock));
        const double t1 = tickAt(p, sg.anchor, static_cast<double>(sg.clock + sg.len));
        for (int i = 0; i < sg.len && sg.from + i < n; ++i)
          cur[sg.from + i] = t0 + (t1 - t0) * static_cast<double>(i) / static_cast<double>(std::max(1, sg.len));
      }
    const size_t lat = static_cast<size_t>(recLatency_.load(std::memory_order_relaxed));
    const size_t mask = kTickHistory - 1;
    // A longer latency reaches further back than any frame read so far: those
    // entries are positions of an earlier (finished) pass, which would replay
    // as a ghost take. Forget them; the new latency starts from silence.
    if (lat > tickHistLat_) {
      for (size_t i = tickHistLat_ + 1; i <= lat; ++i) tickHist_[(tickHistPos_ - i) & mask] = -1.0;
    }
    tickHistLat_ = lat;
    for (int i = 0; i < n; ++i) {
      tickHist_[tickHistPos_ & mask] = cur[i];
      cur[i] = tickHist_[(tickHistPos_ - lat) & mask];
      ++tickHistPos_;
    }
    captureRecording(p, n);
  }
  if (segCount_ == 0) segs_[segCount_++] = {0, n, 0.0, absClock_};  // stopped: free-running clock

  // --- internal controller links, then note processors ---
  // Links are evaluated at the start of each control block of the absolute
  // sample grid; strip gains then ramp across the whole control block, so the
  // result does not depend on how the host splits its buffers.
  const bool grid = p->controlGrid();
  const int gpos = grid ? static_cast<int>(absClock_ % kControlBlock) : 0;
  if (grid && gpos == 0) evaluateLinks(p, kControlBlock, blockTick);
  routePluginMidi(p, n);
  runNoteFx(p, n);

  // --- generators → mixer graph (serial or on the worker pool) ---
  bc_.p = p;
  bc_.n = n;
  bc_.grid = grid;
  bc_.gpos = gpos;
  bc_.nb = std::min(p->mixer.size(), busL_.size());
  bc_.ctx.sampleRate = sr_;
  bc_.ctx.tempo = clockMap(p).bpmAt(blockTick);
  bc_.ctx.ppq = p->ppq;
  bc_.ctx.tick = blockTick;
  bc_.ctx.playing = tPlaying_;
  bc_.ctx.recording = recOn && tPlaying_ && countInLeft_ <= 0;
  bc_.ctx.ports = &ports_;
  ports_.beginBlock();
  {
    const Tick beat = std::max<Tick>(1, p->beatTicks);
    bc_.ctx.tsDen = std::max(1, static_cast<int>(static_cast<Tick>(p->ppq) * 4 / beat));
    bc_.ctx.tsNum = std::max(1, static_cast<int>(std::max<Tick>(1, p->barTicks) / beat));
    bc_.ctx.looping = tPlaying_ && tLoop_ && tMode_ != PlayMode::Live;
    bc_.ctx.loopStart = loopStart(p);
    bc_.ctx.loopEnd = tMode_ == PlayMode::Live ? 0.0 : loopEnd(p);
    bc_.ctx.offline = offline_;
    bc_.ctx.interp = interp_;
    bc_.ctx.jumped = tPlaying_ != ctxWasPlaying_ || std::fabs(blockTick - ctxNextTick_) > 1e-6 * std::max(1.0, blockTick);
    ctxWasPlaying_ = tPlaying_;
    ctxNextTick_ = curTick_;
  }
  bc_.profile = profiling_.load(std::memory_order_relaxed);
  runGraph(p, n);
  captureRecording(p, n, true);  // J-31: tracks recording their output (closes their takes too)
  const float* mL = busL_[static_cast<size_t>(p->master)].data();
  const float* mR = busR_[static_cast<size_t>(p->master)].data();
  logAudio(p, mL, mR, n);
  bool bad = false;
  for (int i = 0; i < n; ++i) {
    L[i] = mL[i];
    R[i] = mR[i];
    bad = bad || !std::isfinite(L[i]) || !std::isfinite(R[i]);
  }
  if (bad) {  // never send NaN/inf to the speakers; reset the DSP that produced it
    std::memset(L, 0, sizeof(float) * static_cast<size_t>(n));
    std::memset(R, 0, sizeof(float) * static_cast<size_t>(n));
    for (const auto& ch : p->channels)
      if (ch.rt && ch.rt->gen) {
        ch.rt->gen->kill();
        for (const auto& nf : ch.noteFx)
          if (nf.rt) nf.rt->fx->reset();
      }
    for (const auto& m : p->mixer)
      for (const auto& s : m.slots)
        if (s.rt && s.rt->fx) s.rt->fx->reset();
  }
  if (clickLeft_ > 0) {  // metronome click
    int from = std::max(0, clickStart_);
    int total = static_cast<int>(sr_ * 0.03);
    for (int i = from; i < n && clickLeft_ > 0; ++i, --clickLeft_) {
      float env = static_cast<float>(clickLeft_) / static_cast<float>(total);
      float v = 0.35f * env * env * static_cast<float>(std::sin(clickPhase_));
      clickPhase_ += 6.283185307179586 * clickFreq_ / sr_;
      L[i] += v;
      R[i] += v;
    }
    clickStart_ = 0;
  }
  if (pvSample_) {  // browser preview, straight to the output
    const Sample& smp = *pvSample_;
    double rate = smp.sampleRate / sr_;
    size_t len = smp.frames();
    for (int i = 0; i < n; ++i) {
      size_t p0 = static_cast<size_t>(pvPos_);
      if (p0 + 1 >= len) {
        pvSample_ = nullptr;
        previewing_.store(false);
        break;
      }
      float fr = static_cast<float>(pvPos_ - static_cast<double>(p0));
      L[i] += 0.8f * (smp.left[p0] + (smp.left[p0 + 1] - smp.left[p0]) * fr);
      R[i] += 0.8f * (smp.right[p0] + (smp.right[p0 + 1] - smp.right[p0]) * fr);
      pvPos_ += rate;
    }
  }
  storeMax(outPeakL_, peakOf(L, n));
  storeMax(outPeakR_, peakOf(R, n));
}

// ---- Live mode: clip launching ----------------------------------------------

namespace {
// Launch grid in ticks (0 = immediately); see launchGridTicks (core/Arrangements).
Tick liveGrid(const Program* p, LaunchGrid g, Tick clipLength) {
  const Tick beat = std::max<Tick>(1, p->beatTicks);
  switch (g) {
    case LaunchGrid::Now: return 0;
    case LaunchGrid::Step: return std::max<Tick>(1, beat / 4);
    case LaunchGrid::HalfBeat: return std::max<Tick>(1, beat / 2);
    case LaunchGrid::Beat: return beat;
    case LaunchGrid::TwoBeats: return beat * 2;
    case LaunchGrid::ClipLength: return std::max<Tick>(1, clipLength);
    default: return beat * 4;
  }
}
Tick nextOnGrid(Tick t, Tick q) { return q > 0 ? (t + q - 1) / q * q : t; }
}  // namespace

void AudioEngine::clearLive() {
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    liveTracks_[t] = LiveTrack{};
    liveActiveUi_[t].store(kNoId, std::memory_order_relaxed);
    liveQueuedUi_[t].store(kNoId, std::memory_order_relaxed);
  }
}

void AudioEngine::liveCommand(const Program* p, const Cmd& c) {
  const double here = tPlaying_ ? tickAt(p, anchorTick_, static_cast<double>(clock_)) : anchorTick_;
  const Tick now = std::max<Tick>(0, static_cast<Tick>(std::ceil(here - 1e-6)));
  auto queueStop = [&](const ProgLiveTrack& lt) {
    LiveTrack& st = liveTracks_[lt.track];
    st.queued = kNoId;
    if (st.active == kNoId) return;
    int li = p->liveClipIndex(st.active);
    st.stopAt = nextOnGrid(now, liveGrid(p, lt.launch, li >= 0 ? p->liveClips[static_cast<size_t>(li)].length : 1));
    st.stopQueued = true;
  };
  if (c.kind == Cmd::LiveStopAll) {
    for (const auto& lt : p->liveTracks) queueStop(lt);
    return;
  }
  if (c.kind == Cmd::LiveStop) {
    int ti = p->liveTrackIndex(c.pitch);
    if (ti >= 0) queueStop(p->liveTracks[static_cast<size_t>(ti)]);
    return;
  }
  const int li = p->liveClipIndex(c.id);
  if (li < 0) return;
  const ProgLiveClip& lc = p->liveClips[static_cast<size_t>(li)];
  const int ti = p->liveTrackIndex(lc.track);
  if (ti < 0 || lc.track < 0 || lc.track >= kMaxPlaylistTracks) return;
  const ProgLiveTrack& lt = p->liveTracks[static_cast<size_t>(ti)];
  LiveTrack& st = liveTracks_[lc.track];
  if (c.kind == Cmd::Release) {  // gate pads stop when released
    if (lt.pad == PadMode::Gate && (st.active == lc.clip || st.queued == lc.clip)) queueStop(lt);
    return;
  }
  if (lt.pad == PadMode::Toggle) {
    if (st.queued == lc.clip) {  // second press before it started: cancel
      st.queued = kNoId;
      return;
    }
    if (st.active == lc.clip && !st.stopQueued) {
      queueStop(lt);
      return;
    }
  }
  // One clip per track: the new clip replaces the playing one at the launch time.
  const Tick at = nextOnGrid(now, liveGrid(p, lt.launch, lc.length));
  Tick origin = at;
  if (lt.syncStart != LaunchGrid::Now) {  // content in phase with the song position
    Tick s = liveGrid(p, lt.syncStart, lc.length);
    if (s > 0) origin = at / s * s;
  }
  st.queued = lc.clip;
  st.queuedAt = at;
  st.queuedOrigin = origin;
  st.stopQueued = false;
}

void AudioEngine::dispatchLive(const Program* p, int from, int to) {
  const int len = to - from;
  if (len <= 0) return;
  const int64_t c0 = clock_, c1 = clock_ + len;
  const double t0 = tickAt(p, anchorTick_, static_cast<double>(c0));
  const double t1 = tickAt(p, anchorTick_, static_cast<double>(c1));
  const double spt = sr_ * 60.0 / (flatBpm(p) * p->ppq);
  auto sampleOf = [&](double tick) { return sampleAt(p, anchorTick_, tick); };
  // Plays content ticks of one clip between transport ticks [a, b).
  auto playPiece = [&](int li, Tick origin, double a, double b) {
    const ProgLiveClip& lc = p->liveClips[static_cast<size_t>(li)];
    const int64_t sa = std::max(c0, sampleOf(a)), sb = std::min(c1, sampleOf(b));
    if (sb <= sa) return;
    const double ca = a - static_cast<double>(origin), cb = b - static_cast<double>(origin);
    auto it = std::lower_bound(lc.notes.begin(), lc.notes.end(), static_cast<Tick>(std::floor(ca)) - 1,
                               [](const ProgLiveClip::Ev& e, Tick t) { return e.e.on < t; });
    for (; it != lc.notes.end() && static_cast<double>(it->e.on) < cb + 1.0; ++it) {
      const int64_t s = sampleOf(static_cast<double>(origin + it->e.on));
      if (s < sa || s >= sb) continue;
      if (it->channel < 0 || static_cast<size_t>(it->channel) >= p->channels.size()) continue;
      const ProgChannel& ch = p->channels[static_cast<size_t>(it->channel)];
      if (!ch.rt || !ch.rt->gen || ch.rt->pendingCount >= kMaxPendingNotes) continue;
      NoteEv x = it->e;
      x.on += origin;
      x.off += origin;
      startNote(p, ch, x, from + static_cast<int>(s - c0), absClock_ + from + (s - c0), spt);
    }
    if (!lc.audio.empty() && livePieceCount_ < kMaxLivePieces)
      livePieces_[livePieceCount_++] = {li, from + static_cast<int>(sa - c0), static_cast<int>(sb - sa),
                                        anchorTick_ - static_cast<double>(origin), sa};
  };
  for (const ProgLiveTrack& lt : p->liveTracks) {
    if (lt.track < 0 || lt.track >= kMaxPlaylistTracks || lt.clips.empty()) continue;
    LiveTrack& st = liveTracks_[lt.track];
    double cur = t0;
    for (int guard = 0; guard < 64 && (st.active != kNoId || st.queued != kNoId); ++guard) {
      int li = st.active != kNoId ? p->liveClipIndex(st.active) : -1;
      if (st.active != kNoId && li < 0) st.active = kNoId;  // the clip was deleted
      double next = t1;
      if (st.queued != kNoId) next = std::min(next, std::max(cur, static_cast<double>(st.queuedAt)));
      if (st.stopQueued) next = std::min(next, std::max(cur, static_cast<double>(st.stopAt)));
      Tick endT = 0;
      if (li >= 0) {
        endT = st.origin + p->liveClips[static_cast<size_t>(li)].length;
        next = std::min(next, std::max(cur, static_cast<double>(endT)));
      }
      if (li >= 0 && next > cur) playPiece(li, st.origin, cur, next);
      cur = next;
      if (cur >= t1) break;  // transitions at t1 happen in the next segment
      // Transitions due at `cur`.
      if (st.stopQueued && static_cast<double>(st.stopAt) <= cur + 1e-9) {
        st.active = kNoId;
        st.stopQueued = false;
      }
      if (st.queued != kNoId && static_cast<double>(st.queuedAt) <= cur + 1e-9) {
        st.active = st.queued;
        st.origin = st.queuedOrigin;
        st.queued = kNoId;
      } else if (li >= 0 && st.active == p->liveClips[static_cast<size_t>(li)].clip && cur >= static_cast<double>(endT) - 1e-9) {
        // Follow action at the clip end.
        const int n = static_cast<int>(lt.clips.size());
        int pos = 0;
        for (int k = 0; k < n; ++k)
          if (lt.clips[static_cast<size_t>(k)] == li) pos = k;
        int nextPos = -1;
        switch (lt.follow) {
          case FollowMode::Loop: nextPos = pos; break;
          case FollowMode::Once: nextPos = -1; break;
          case FollowMode::Next: nextPos = (pos + 1) % n; break;
          case FollowMode::NextHold: nextPos = std::min(pos + 1, n - 1); break;
          case FollowMode::NextStop: nextPos = pos + 1 < n ? pos + 1 : -1; break;
          case FollowMode::Random:
          case FollowMode::Shuffle:
            liveRng_ = liveRng_ * 1664525u + 1013904223u;
            nextPos = static_cast<int>((liveRng_ >> 8) % static_cast<uint32_t>(n));
            if (lt.follow == FollowMode::Shuffle && n > 1 && nextPos == pos) nextPos = (nextPos + 1) % n;
            break;
          default: nextPos = pos; break;
        }
        if (nextPos < 0) {
          st.active = kNoId;
        } else {
          st.active = p->liveClips[static_cast<size_t>(lt.clips[static_cast<size_t>(nextPos)])].clip;
          st.origin = endT;
        }
      }
    }
  }
  for (const ProgLiveTrack& lt : p->liveTracks)
    if (lt.track >= 0 && lt.track < kMaxPlaylistTracks) {
      liveActiveUi_[lt.track].store(liveTracks_[lt.track].active, std::memory_order_relaxed);
      liveQueuedUi_[lt.track].store(liveTracks_[lt.track].queued, std::memory_order_relaxed);
    }
}

void AudioEngine::evaluateLinks(const Program* p, int n, double blockTick) {
  double vars[4] = {0, 0, static_cast<double>(absClock_) / sr_, blockTick / std::max(1, p->ppq)};
  for (const ProgLink& l : p->links) {
    float src = 0;
    if (l.srcChannel >= 0) {
      const ChannelRuntime* r = p->channels[static_cast<size_t>(l.srcChannel)].rt.get();
      if (!r) continue;
      src = r->ctl[l.output];
    } else {
      const EffectRuntime* r = p->mixer[static_cast<size_t>(l.srcMixer)].slots[static_cast<size_t>(l.srcSlot)].rt.get();
      if (!r) continue;
      src = r->ctl[l.output];
    }
    LinkRuntime& lr = *l.rt;
    double y;
    if (l.curve == ModCurve::Formula) {
      vars[0] = src;
      vars[1] = l.base;
      y = l.formula.eval(vars, &lr.rng);
    } else {
      y = applyModCurve(l.curve, src);
    }
    y = y < 0 ? 0 : (y > 1 ? 1 : y);
    double target = l.min + y * (l.max - l.min);
    if (lr.value < 0 || l.smoothMs <= 0) {
      lr.value = target;
    } else {
      double coef = 1.0 - std::exp(-static_cast<double>(n) / (l.smoothMs * 0.001 * sr_));
      lr.value += (target - lr.value) * coef;
    }
    applyLinkValue(p, l, lr.value);
  }
  // PH-09: host macros (the plugin's host parameters), before automation clips.
  if (p->hasMacros)
    for (size_t k = 0; k < p->macros.size(); ++k) {
      const float v = macro_[k].load(std::memory_order_relaxed);
      if (p->macros[k].valid && v >= 0.f) applyLinkValue(p, p->macros[k].target, v > 1.f ? 1.0 : static_cast<double>(v));
    }
  // Automation clips (arrangement): the value at this control block's song
  // position; applied after the controller links, so automation wins.
  if (tMode_ == PlayMode::Song)
    for (const ProgAutoLane& a : p->autos) applyLinkValue(p, a.target, a.lane.valueAt(blockTick));
  // Pattern automation: the selected pattern (pattern mode) or the launched
  // clips (live mode), at their content position.
  if (tMode_ == PlayMode::Pattern) {
    if (const ProgPattern* pat = p->pattern(tPattern_))
      for (const ProgAutoLane& a : pat->autos) applyLinkValue(p, a.target, a.lane.valueAt(blockTick));
  } else if (tMode_ == PlayMode::Live && tPlaying_) {
    for (const ProgLiveTrack& lt : p->liveTracks) {
      const LiveTrack& st = liveTracks_[lt.track];
      if (st.active == kNoId) continue;
      int li = p->liveClipIndex(st.active);
      if (li < 0) continue;
      const double rel = blockTick - static_cast<double>(st.origin);
      if (rel < 0) continue;
      for (const ProgAutoLane& a : p->liveClips[static_cast<size_t>(li)].autos) applyLinkValue(p, a.target, a.lane.valueAt(rel));
    }
  }
  // Strip gains from the (possibly linked) volume / pan / mute.
  for (const auto& c : p->channels) {
    if (!c.stripLinked) continue;
    double g = c.modMuted ? 0.0 : dsp::faderGain(c.modVolume, 0.78), lg, rg;
    dsp::panGains(c.modPan, lg, rg);
    c.gainL = static_cast<float>(g * lg);
    c.gainR = static_cast<float>(g * rg);
  }
  for (const auto& m : p->mixer) {
    if (!m.stripLinked) continue;
    double g = dsp::faderGain(m.modVolume, 0.8), lg, rg;
    dsp::panGains(m.modPan, lg, rg);
    m.gainL = static_cast<float>(g * lg);
    m.gainR = static_cast<float>(g * rg);
  }
}

// Writes a link / automation value (normalized) into the program's live state.
void AudioEngine::applyLinkValue(const Program* p, const ProgLink& l, double normalized) {
  {
    double v = l.desc.fromNormalized(normalized);
    if (l.rt) {
      l.rt->display.store(static_cast<float>(v), std::memory_order_relaxed);
      l.rt->active.store(true, std::memory_order_relaxed);
    }
    float fv = static_cast<float>(v);
    switch (l.kind) {
      case ProgLink::ChanPlugin: {
        const ProgChannel& c = p->channels[static_cast<size_t>(l.chan)];
        if (static_cast<size_t>(l.param) < c.live.params.size()) c.live.params[static_cast<size_t>(l.param)] = fv;
        break;
      }
      case ProgLink::ChanNoteFx: {
        const ProgNoteFx& f = p->channels[static_cast<size_t>(l.chan)].noteFx[static_cast<size_t>(l.fx)];
        if (static_cast<size_t>(l.param) < f.live.params.size()) f.live.params[static_cast<size_t>(l.param)] = fv;
        break;
      }
      case ProgLink::ChanVolume: p->channels[static_cast<size_t>(l.chan)].modVolume = fv; break;
      case ProgLink::ChanPan: p->channels[static_cast<size_t>(l.chan)].modPan = fv; break;
      case ProgLink::ChanMute: p->channels[static_cast<size_t>(l.chan)].modMuted = v >= 0.5; break;
      case ProgLink::ChanPitch: {
        const ProgChannel& c = p->channels[static_cast<size_t>(l.chan)];
        c.pitchOffset = static_cast<float>(std::lround(v)) + c.masterPitch;
        break;
      }
      case ProgLink::MixVolume: p->mixer[static_cast<size_t>(l.mixer)].modVolume = fv; break;
      case ProgLink::MixPan: p->mixer[static_cast<size_t>(l.mixer)].modPan = fv; break;
      case ProgLink::MixSep: p->mixer[static_cast<size_t>(l.mixer)].sep = fv; break;
      case ProgLink::MixMute: p->mixer[static_cast<size_t>(l.mixer)].muteMod = v >= 0.5; break;
      case ProgLink::SlotParam: {
        const ProgSlot& s = p->mixer[static_cast<size_t>(l.mixer)].slots[static_cast<size_t>(l.slot)];
        if (static_cast<size_t>(l.param) < s.live.params.size()) s.live.params[static_cast<size_t>(l.param)] = fv;
        break;
      }
      case ProgLink::SlotMix: p->mixer[static_cast<size_t>(l.mixer)].slots[static_cast<size_t>(l.slot)].mix = fv; break;
      case ProgLink::SlotEnable: p->mixer[static_cast<size_t>(l.mixer)].slots[static_cast<size_t>(l.slot)].enabled = v >= 0.5; break;
      case ProgLink::MixEqGain: p->mixer[static_cast<size_t>(l.mixer)].eq.gain[static_cast<size_t>(l.param)] = fv; break;
      case ProgLink::MixEqFreq: p->mixer[static_cast<size_t>(l.mixer)].eq.freq[static_cast<size_t>(l.param)] = fv; break;
      case ProgLink::MixEqQ: p->mixer[static_cast<size_t>(l.mixer)].eq.q[static_cast<size_t>(l.param)] = fv; break;
      case ProgLink::MixSend: p->mixer[static_cast<size_t>(l.mixer)].sends[static_cast<size_t>(l.param)].second = fv; break;
    }
  }
}

void AudioEngine::runNoteFx(const Program* p, int n) {
  for (size_t ci = 0; ci < p->channels.size(); ++ci) {
    const ProgChannel& ch = p->channels[ci];
    ChannelRuntime* rt = ch.rt.get();
    if (!rt || !rt->gen) continue;
    uintptr_t sig = 0;
    for (const auto& nf : ch.noteFx) sig = sig * 31u + reinterpret_cast<uintptr_t>(nf.rt.get());
    if (sig != rt->chainSig) {  // chain added, removed or rebuilt: release what it was holding
      if (rt->chainSig != 0) rt->gen->allNotesOff(0), rt->keys.clear();
      for (const auto& nf : ch.noteFx)
        if (nf.rt) nf.rt->fx->reset();
      rt->chainSig = sig;
    }
    if (sig == 0) {
      rt->inbox.clear();
      continue;
    }
    const double spt = sr_ * 60.0 / (flatBpm(p) * p->ppq);
    for (int sgi = 0; sgi < segCount_; ++sgi) {
      const Segment& sg = segs_[sgi];
      bool last = sgi == segCount_ - 1;
      NoteBuffer* in = &fxA_;
      NoteBuffer* out = &fxB_;
      in->clear();
      for (int e = 0; e < rt->inbox.count; ++e) {
        const NoteMsg& m = rt->inbox.ev[e];
        if (m.offset >= sg.from && (last || m.offset < sg.from + sg.len)) {
          NoteMsg x = m;
          x.offset = std::clamp(x.offset, sg.from, std::max(sg.from, sg.from + sg.len - 1));
          in->push(x);
        }
      }
      in->sort();
      NoteFxContext ctx;
      ctx.sampleRate = sr_;
      ctx.tempo = flatBpm(p);
      ctx.ppq = p->ppq;
      ctx.from = sg.from;
      ctx.len = sg.len;
      ctx.anchorTick = sg.anchor;
      ctx.clock = sg.clock;
      ctx.spt = spt;
      if (!clockMap(p).constant()) {  // tempo map: the processors see the local tempo of this segment
        ctx.anchorTick = tickAt(p, sg.anchor, static_cast<double>(sg.clock));
        ctx.clock = 0;
        ctx.tempo = clockMap(p).bpmAt(ctx.anchorTick);
        ctx.spt = sr_ * 60.0 / (ctx.tempo * p->ppq);
      }
      ctx.playing = tPlaying_;
      // Per-channel id range (J-30): 1M ids per channel before ranges meet.
      if (rt->fxIdSeq == 0) rt->fxIdSeq = ((static_cast<uint32_t>(ci) & 0xFFu) << 20) | 1u;
      ctx.idCounter = &rt->fxIdSeq;
      for (const auto& nf : ch.noteFx) {
        if (!nf.rt) continue;
        out->clear();
        nf.rt->fx->process(nf.modded ? nf.live : nf.state, nf.aux, ctx, *in, *out);
        out->sort();
        nf.rt->position.store(nf.rt->fx->position(), std::memory_order_relaxed);
        std::swap(in, out);
      }
      for (int e = 0; e < in->count; ++e) {
        const NoteMsg& m = in->ev[e];
        const ProgChannel* tc = &ch;
        if (m.target >= 0 && static_cast<size_t>(m.target) < p->channels.size()) tc = &p->channels[static_cast<size_t>(m.target)];
        Generator* g = tc->rt && tc->rt->gen ? tc->rt->gen.get() : nullptr;
        if (!g) continue;
        int off = std::clamp(m.offset, 0, n - 1);
        if (m.kind == NoteMsg::On) {
          NoteProps np;
          np.pan = m.pan;
          np.release = m.release;
          np.modX = m.modX;
          np.modY = m.modY;
          np.color = m.color;
          if (np.neutral())
            g->noteOn(m.id, m.pitch + tc->pitchOffset, m.velocity, off);
          else
            g->noteOn(m.id, m.pitch + tc->pitchOffset, m.velocity, off, np);
          tc->rt->keys.on(m.id, m.pitch, m.velocity);  // J-77: the notes the chain plays light up
        }
        else if (m.kind == NoteMsg::Off)
          g->noteOff(m.id, off), tc->rt->keys.off(m.id);
        else
          g->allNotesOff(off), tc->rt->keys.clear();
      }
    }
    rt->inbox.clear();
  }
}

void AudioEngine::setFixedBlock(int frames) {
  fixed_ = std::clamp(frames, 0, kMaxBlock);
  fifoL_.assign(static_cast<size_t>(std::max(1, fixed_)), 0.f);
  fifoR_.assign(static_cast<size_t>(std::max(1, fixed_)), 0.f);
  fifoPos_ = fifoLen_ = 0;
}

void AudioEngine::addBlockBarrier(int64_t sample) {
  if (barrierCount_ < kMaxBarriers) barriers_[barrierCount_++] = sample;
}

void AudioEngine::process(float* L, float* R, int frames, const OutputTaps* taps) {
  if (fixed_ <= 0) {
    taps_ = taps;
    processDirect(L, R, frames);
    taps_ = nullptr;
    return;
  }
  if (taps)
    for (int k = 0; k < taps->count; ++k) {
      std::memset(taps->L[k], 0, sizeof(float) * static_cast<size_t>(frames));
      std::memset(taps->R[k], 0, sizeof(float) * static_cast<size_t>(frames));
    }
  // J-30: render whole blocks on the engine's own grid and hand them out in the
  // caller's chunk sizes. Every internal block boundary is then a function of
  // the absolute sample position alone (plus the barriers), so per-block state
  // in instruments, effects and hosted plugins sees the same blocks for any
  // caller block size.
  int done = 0;
  while (done < frames) {
    if (fifoPos_ == fifoLen_) {
      int len = fixed_ - static_cast<int>(absClock_ % fixed_);
      for (int b = 0; b < barrierCount_; ++b)
        if (barriers_[b] > absClock_ && barriers_[b] < absClock_ + len) len = static_cast<int>(barriers_[b] - absClock_);
      processDirect(fifoL_.data(), fifoR_.data(), len);
      fifoPos_ = 0;
      fifoLen_ = len;
    }
    const int k = std::min(fifoLen_ - fifoPos_, frames - done);
    std::memcpy(L + done, fifoL_.data() + fifoPos_, sizeof(float) * static_cast<size_t>(k));
    std::memcpy(R + done, fifoR_.data() + fifoPos_, sizeof(float) * static_cast<size_t>(k));
    fifoPos_ += k;
    done += k;
  }
}

void AudioEngine::processDirect(float* L, float* R, int frames) {
  const ScopedFlushDenormals ftz;  // J-25: denormal tails cost ~100x on x86
  auto t0 = std::chrono::steady_clock::now();
  blockSampleUi_.store(absClock_, std::memory_order_relaxed);
  blockTimeUi_.store(std::chrono::duration<double>(t0.time_since_epoch()).count(), std::memory_order_relaxed);
  int done = 0;
  while (done < frames) {
    int n = std::min(kMaxBlock, frames - done);
    const Program* p = live_.load();
    // With controller links, render on a fixed grid so link evaluation (once
    // per control block) is independent of the audio buffer size.
    if (p && p->controlGrid())
      n = std::min(n, kControlBlock - static_cast<int>(absClock_ % kControlBlock));
    drain(p);
    drainMidi(p);
    if (done == 0 && hostNew_) followHost(p);
    processBlock(p, L + done, R + done, n);
    if (taps_) copyTaps(p, done, n);
    absClock_ += n;
    blocks_.fetch_add(1);
    done += n;
  }
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  double avail = frames / sr_;
  storeMax(cpuPeak_, cpuMeter_.add(el, avail));
  cpu_.store(cpuMeter_.load(), std::memory_order_relaxed);
}

void AudioEngine::copyTaps(const Program* p, int offset, int n) {
  const size_t bytes = sizeof(float) * static_cast<size_t>(n);
  for (int k = 0; k < taps_->count; ++k) {
    float* l = taps_->L[k] + offset;
    float* r = taps_->R[k] + offset;
    const int mi = taps_->mixer ? taps_->mixer[k] : -1;
    if (p && mi >= 0 && static_cast<size_t>(mi) < p->mixer.size() && static_cast<size_t>(mi) < busL_.size()) {
      std::memcpy(l, busL_[static_cast<size_t>(mi)].data(), bytes);
      std::memcpy(r, busR_[static_cast<size_t>(mi)].data(), bytes);
    } else {
      std::memset(l, 0, bytes);
      std::memset(r, 0, bytes);
    }
  }
}

// PH-09: the transport follows the host (see HostTransport).
void AudioEngine::followHost(const Program* p) {
  hostNew_ = false;
  const HostTransport h = hostIn_;
  hostFollow_ = h.follow && p;
  hostFollowUi_.store(hostFollow_, std::memory_order_relaxed);
  if (!hostFollow_) return;
  if (h.bpm > 0 && (h.bpm != hostBpm_ || hostMap_.ppq() != std::max(1, p->ppq))) {
    hostBpm_ = std::clamp(h.bpm, 1.0, 1000.0);
    hostMap_.setConstant(hostBpm_, p->ppq);
  }
  // A new tempo continues from the current position (as drain does for tempo edits).
  const uint64_t sig = clockMap(p).signature();
  if (sig != lastMapSig_) {
    if (lastMapSig_ != 0 && clock_ != 0) {
      anchorTick_ = curTick_;
      clock_ = 0;
    }
    lastMapSig_ = sig;
  }
  const double here = tickAt(p, anchorTick_, static_cast<double>(clock_));
  double target = here;
  double wrap = 0;  // length the transport wraps at (0 = none)
  if (h.hasPosition && tMode_ != PlayMode::Live) {
    target = h.beats * static_cast<double>(p->ppq);
    if (tLoop_) wrap = transportEnd(p);
    if (wrap > 0) {
      target = std::fmod(target, wrap);
      if (target < 0) target += wrap;
    }
  }
  const double bpm = clockMap(p).bpmAt(here);
  const double tol = 2.0 * bpm * p->ppq / (60.0 * sr_) + 1e-6;  // two samples
  if (h.playing && !tPlaying_) {
    releaseAll(p, 0, true);
    landAt(target);
    startTick_ = target;
    tPlaying_ = true;
    countInLeft_ = 0;
    countingIn_.store(false);
    pendJump_ = false;
  } else if (!h.playing && tPlaying_) {
    releaseAll(p, 0, true);
    tPlaying_ = false;
    landAt(target);
    pendJump_ = false;
  } else if (h.hasPosition && tMode_ != PlayMode::Live) {
    double d = here - target;
    if (wrap > 0) d = std::remainder(d, wrap);  // the same place one loop apart
    if (std::fabs(d) > tol) {
      if (tPlaying_) releaseAll(p, 0);
      landAt(target);
      pendJump_ = false;
    }
  }
  if (!tPlaying_) {
    curTick_ = tickAt(p, anchorTick_, static_cast<double>(clock_));
    posTick_.store(curTick_);
  }
}

void AudioEngine::processInterleaved(float* out, int frames) {
  int done = 0;
  while (done < frames) {
    int n = std::min(kMaxBlock, frames - done);
    process(tmpL_.data(), tmpR_.data(), n);
    for (int i = 0; i < n; ++i) {
      out[2 * (done + i)] = tmpL_[static_cast<size_t>(i)];
      out[2 * (done + i) + 1] = tmpR_[static_cast<size_t>(i)];
    }
    done += n;
  }
}

}  // namespace gl

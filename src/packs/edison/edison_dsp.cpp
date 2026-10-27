// Gridline — "edison" pack: mixer-slot runtime (input capture for recording,
// playback of the edited audio, transport follow), the recorder that turns
// captured input into takes, and the pack entry points.
#include <algorithm>
#include <cmath>
#include "../../engine/Dsp.h"
#include "../../engine/pitch/Kernel.h"
#include "../Packs.h"
#include "edison.h"

namespace gl::packs::edison {

using namespace gl::dsp;

// ------------------------------------------------------------------ capture ring
CaptureRing::CaptureRing() : L_(kSize, 0.f), R_(kSize, 0.f), play_(kSize, 0) {}

void CaptureRing::write(const float* L, const float* R, int n, bool playing) {
  size_t w = pos_.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    const size_t k = (w + static_cast<size_t>(i)) & (kSize - 1);
    L_[k] = L[i];
    R_[k] = R[i];
    play_[k] = playing ? 1 : 0;
  }
  pos_.store(w + static_cast<size_t>(n), std::memory_order_release);
}

size_t CaptureRing::read(size_t& from, size_t maxN, float* L, float* R, uint8_t* playing) const {
  const size_t w = pos_.load(std::memory_order_acquire);
  if (w > kSize && from < w - kSize) from = w - kSize;  // overwritten: skip ahead
  if (from > w) from = w;
  const size_t n = std::min(maxN, w - from);
  for (size_t i = 0; i < n; ++i) {
    const size_t k = (from + i) & (kSize - 1);
    L[i] = L_[k];
    R[i] = R_[k];
    if (playing) playing[i] = play_[k];
  }
  from += n;
  return n;
}

// ------------------------------------------------------------------ recorder
void Recorder::arm(const Config& c) {
  cfg_ = c;
  armed_ = true;
  recording_ = ready_ = paused_ = false;
  quiet_ = 0;
  L_.clear();
  R_.clear();
  thr_ = dbToGain(c.thresholdDb);
  const size_t pre = static_cast<size_t>(std::max(0.0, c.preRollSec) * c.rate);
  if (histL_.size() != std::max<size_t>(1, pre)) {
    histL_.assign(std::max<size_t>(1, pre), 0.f);
    histR_.assign(histL_.size(), 0.f);
    histPos_ = histFill_ = 0;
  }
  if (c.mode == RecNow) start(pre);
}

void Recorder::disarm() {
  armed_ = recording_ = ready_ = paused_ = false;
  L_.clear();
  R_.clear();
}

void Recorder::stop() {
  if (recording_) ready_ = true;
  recording_ = armed_ = paused_ = false;
}

void Recorder::start(size_t pre) {
  recording_ = true;
  paused_ = false;
  quiet_ = 0;
  if (cfg_.preRollSec <= 0) return;
  pre = std::min(pre, histFill_);
  const size_t sz = histL_.size();
  for (size_t i = 0; i < pre; ++i) {
    const size_t k = (histPos_ + sz - pre + i) % sz;
    L_.push_back(histL_[k]);
    R_.push_back(histR_[k]);
  }
}

void Recorder::push(float l, float r) {
  L_.push_back(l);
  R_.push_back(r);
}

void Recorder::feed(const float* L, const float* R, const uint8_t* playing, size_t n) {
  const size_t pre = static_cast<size_t>(std::max(0.0, cfg_.preRollSec) * cfg_.rate);
  const int64_t maxFrames = cfg_.maxSec > 0 ? static_cast<int64_t>(cfg_.maxSec * cfg_.rate) : 0;
  const int64_t hold = static_cast<int64_t>(cfg_.gateHoldSec * cfg_.rate);
  for (size_t i = 0; i < n; ++i) {
    const float l = L[i], r = R[i];
    const bool play = playing && playing[i];
    const bool loud = std::fabs(l) >= thr_ || std::fabs(r) >= thr_;
    if (armed_ && !recording_) {
      bool trig = false;
      if (cfg_.mode == RecOnInput || cfg_.mode == RecInput) trig = loud;
      else if (cfg_.mode == RecOnPlay) trig = play;
      else if (cfg_.mode == RecNow) trig = true;
      if (trig) {
        start(pre);
        push(l, r);
      } else {
        // Pre-roll history.
        histL_[histPos_] = l;
        histR_[histPos_] = r;
        histPos_ = (histPos_ + 1) % histL_.size();
        histFill_ = std::min(histFill_ + 1, histL_.size());
      }
    } else if (recording_) {
      if (cfg_.mode == RecOnPlay && wasPlaying_ && !play) {
        stop();
      } else if (cfg_.mode == RecInput) {
        if (loud) {
          quiet_ = 0;
          paused_ = false;
        } else if (++quiet_ > hold) {
          paused_ = true;
        }
        if (!paused_) push(l, r);
      } else {
        push(l, r);
      }
      if (maxFrames > 0 && static_cast<int64_t>(L_.size()) >= maxFrames) {
        L_.resize(static_cast<size_t>(maxFrames));
        R_.resize(static_cast<size_t>(maxFrames));
        stop();
      }
    }
    wasPlaying_ = play;
  }
}

Audio Recorder::take(int channels) {
  Audio a;
  a.rate = cfg_.rate;
  a.channels = channels == 1 ? 1 : 2;
  if (!ready_) return a;
  a.L = std::move(L_);
  a.R = std::move(R_);
  L_.clear();
  R_.clear();
  ready_ = false;
  if (a.channels == 1) {
    for (size_t i = 0; i < a.L.size(); ++i) a.L[i] = 0.5f * (a.L[i] + a.R[i]);
    a.R = a.L;
  }
  return a;
}

// ------------------------------------------------------------------ effect
EdisonFx::EdisonFx() = default;

void EdisonFx::reset() {
  rplaying_ = false;
  playing_.store(false);
  scrubVel_ = 0;
}

void EdisonFx::stopSound() {
  startReq_.store(false);
  rplaying_ = false;
  playing_.store(false);
}

void EdisonFx::play(int64_t from, int64_t to, bool loop) {
  from_.store(std::max<int64_t>(0, from));
  to_.store(to);
  loop_.store(loop);
  startReq_.store(true);
  playing_.store(true);
}

void EdisonFx::stop() {
  stopReq_.store(true);
  playing_.store(false);
}

void EdisonFx::scrub(bool on, double target) {
  scrubTarget_.store(target);
  if (on && !scrubbing_.load()) scrubJump_.store(true);  // the audio thread jumps there on the first scrub block
  scrubbing_.store(on);
}

void EdisonFx::process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) {
  run(state, nullptr, ctx, L, R, n);
}

void EdisonFx::processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) {
  run(state, aux.samples.empty() ? nullptr : aux.samples[0].get(), ctx, L, R, n);
}

void EdisonFx::run(const PluginState& state, const Sample* src, const ProcessContext& ctx, float* L, float* R, int n) {
  auto P = [&state](int k, float d) { return k < static_cast<int>(state.params.size()) ? state.params[static_cast<size_t>(k)] : d; };
  ring_.write(L, R, n, ctx.playing);
  float ip = 0;
  for (int i = 0; i < n; ++i) ip = std::max({ip, std::fabs(L[i]), std::fabs(R[i])});
  if (ip > inPeak_.load(std::memory_order_relaxed)) inPeak_.store(ip, std::memory_order_relaxed);

  const int64_t frames = src ? static_cast<int64_t>(src->frames()) : 0;
  srcFrames_.store(frames, std::memory_order_relaxed);
  if (stopReq_.exchange(false)) rplaying_ = false;
  if (startReq_.exchange(false)) {
    rpos_ = static_cast<double>(from_.load());
    rplaying_ = true;
  }
  if (P(EdFollowHost, 0) >= 0.5f) {
    if (ctx.playing && !hostWasPlaying_) {
      rpos_ = static_cast<double>(from_.load());
      rplaying_ = true;
    } else if (!ctx.playing && hostWasPlaying_) {
      rplaying_ = false;
    }
  }
  hostWasPlaying_ = ctx.playing;
  const bool mute = P(EdMuteInput, 0) >= 0.5f;
  const float g = static_cast<float>(dbToGain(P(EdOutGain, 0)));
  const bool scrub = scrubbing_.load();
  if (!src || frames < 2) {
    if (mute) std::fill(L, L + n, 0.f), std::fill(R, R + n, 0.f);
    playing_.store(false);
    return;
  }
  const double step = src->sampleRate / std::max(1.0, ctx.sampleRate);
  int64_t to = to_.load();
  const int64_t from = std::clamp<int64_t>(from_.load(), 0, frames - 1);
  if (to <= from || to > frames) to = frames;
  const bool loop = loop_.load() || P(EdLoop, 0) >= 0.5f;
  double vTarget = 0;
  if (scrub) {
    // Scrubbing: move towards the mouse position within ~40 ms, at most 4x speed.
    const double target = scrubTarget_.load();
    if (scrubJump_.exchange(false)) rpos_ = std::clamp(target, 0.0, static_cast<double>(frames - 1)), scrubVel_ = 0;
    vTarget = std::clamp((target - rpos_) / (0.04 * src->sampleRate), -4.0, 4.0) * step;
  }
  float op = 0;
  for (int i = 0; i < n; ++i) {
    float l = mute ? 0.f : L[i], r = mute ? 0.f : R[i];
    if (scrub || rplaying_) {
      // J-140: band-limited read at the playback or scrub speed (was linear:
      // other-rate files and fast scrubs aliased); whole frames at 1:1 stay exact.
      if (rpos_ >= 0 && rpos_ < static_cast<double>(frames)) {
        const double rate = scrub ? std::fabs(scrubVel_) : step;
        float sl, sr;
        if (pitch::exactFrame(rpos_, rate)) {
          const auto u = static_cast<size_t>(std::min<int64_t>(frames - 1, std::llround(rpos_)));
          sl = src->left[u], sr = src->right[u];
        } else {
          pitch::read2(src->left.data(), src->right.data(), frames, rpos_, rate, pitch::table(pitch::Quality::Realtime), sl, sr);
        }
        l += g * sl;
        r += g * sr;
      }
      if (scrub) {
        scrubVel_ += (vTarget - scrubVel_) * 0.002;
        rpos_ = std::clamp(rpos_ + scrubVel_, 0.0, static_cast<double>(frames - 1));
      } else {
        rpos_ += step;
        if (rpos_ >= static_cast<double>(to)) {
          if (loop && to > from)
            rpos_ = static_cast<double>(from) + std::fmod(rpos_ - static_cast<double>(to), static_cast<double>(to - from));
          else
            rplaying_ = false;
        }
      }
    }
    L[i] = l;
    R[i] = r;
    op = std::max({op, std::fabs(l), std::fabs(r)});
  }
  if (!scrub) scrubVel_ = 0;
  if (op > outPeak_.load(std::memory_order_relaxed)) outPeak_.store(op, std::memory_order_relaxed);
  pos_.store(rpos_);
  playing_.store(rplaying_);
}

// ------------------------------------------------------------------ pack entry points
std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == kType) return std::make_unique<EdisonFx>();
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  State s;
  decodeState(state.data, s);
  aux.samples.clear();
  if (!s.path.empty()) aux.samples.push_back(sampleFor(s.path));
  return true;
}

}  // namespace gl::packs::edison

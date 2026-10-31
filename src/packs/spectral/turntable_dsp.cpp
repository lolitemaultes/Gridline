// Gridline — "spectral" pack: Turntable (gl.turntable) runtime. See
// turntable.h for the model. Everything runs per sample; the coefficients are
// defined at 44.1 kHz and converted for other rates, so the platter feels the
// same at any engine rate. No allocation on the audio thread: the sample is
// prepared on the UI thread (prepareTurntableAux) and only read here.
#include <algorithm>
#include <array>
#include <cmath>
#include "../samplers/samplers.h"
#include "turntable.h"

namespace gl::packs::spectral {

namespace {

constexpr double kRefRate = 44100.0;

struct Params {
  const PluginState* s;
  double operator[](int k) const {
    if (s && k >= 0 && static_cast<size_t>(k) < s->params.size()) return s->params[static_cast<size_t>(k)];
    const PluginInfo* info = findPlugin("gl.turntable");
    return info && static_cast<size_t>(k) < info->params.size() ? info->params[static_cast<size_t>(k)].def : 0.0;
  }
};

class TurntableGen final : public Generator {
 public:
  TurntableGen() = default;

  // Sounding while a key is held, the motor runs, the hand holds the platter
  // or the platter / output is still settling.
  bool active() const override {
    if (!smp_) return false;
    return keys_ > 0 || holdParam_ || (transport_ != 0 && speed_ > 0) || std::fabs(vel_) > 1e-9 || g_ > 1e-5;
  }
  const TurntableTelemetry& tel() const { return tel_; }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    (void)velocity;
    if (notesMode_ == TnOff) return;
    // Last-note priority over a small stack of held keys.
    removeKey(id);
    if (keys_ < static_cast<int>(keyIds_.size())) {
      keyIds_[static_cast<size_t>(keys_)] = id;
      keyPitch_[static_cast<size_t>(keys_)] = pitch;
      ++keys_;
    }
    if (notesMode_ == TnCue && smp_) {
      // Start at the cue at full speed: the old read position fades out under the new one.
      fadeFrom_ = pos_;
      fadeVel_ = vel_;
      fadeLeft_ = fadeLen_;
      pos_ = cue_;
      vel_ = motorTarget(pitch);
    }
  }
  void onNoteOff(uint32_t id) override { removeKey(id); }
  void onAllOff() override { keys_ = 0; }
  void onKill() override {
    keys_ = 0;
    g_ = 0;
    vel_ = 0;
    dcx_ = {0, 0};
    dcy_ = {0, 0};
    fadeLeft_ = 0;
  }

  void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) override {
    Params p{&state};
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : kRefRate;
    smp_ = aux.sample(0).get();
    if (smp_ && smp_->frames() < 2) smp_ = nullptr;
    if (smp_ != lastSmp_) {  // a new sample: start from its beginning
      lastSmp_ = smp_;
      pos_ = 0;
      fadeLeft_ = 0;
    }
    const double r = kRefRate / sr_;  // 44.1 kHz samples per engine sample
    position_ = std::clamp(p[TtPosition], 0.0, 1.0) + std::round(std::clamp(p[TtTurns], -9.0, 9.0));
    mute_ = p[TtMute] >= 0.5;
    speed_ = std::clamp(p[TtSpeed], 0.0, 2.0);
    pan_ = std::clamp(p[TtPan], -1.0, 1.0);
    volume_ = std::clamp(p[TtVolume], 0.0, 1.0);
    holdParam_ = p[TtHold] >= 0.5;
    transport_ = std::clamp(static_cast<int>(std::lround(p[TtTransport])), 0, 2) - 1;
    notesMode_ = std::clamp(static_cast<int>(std::lround(p[TtNotes])), 0, 2);
    keyTrack_ = p[TtKeyTrack] >= 0.5;
    cue_ = std::clamp(p[TtCue], 0.0, 1.0);
    // Motor inertia: one-pole per 44.1 kHz sample, 0.99999 (Acceleration 0) .. 0.99949 (100 %).
    motorA_ = std::pow(0.99999 - 0.0005 * std::clamp(p[TtAccel], 0.0, 1.0), r);
    // Hand spring (defined per 44.1 kHz sample, see turntable.h).
    const double s1 = std::clamp(p[TtSmooth1], 0.0, 1.0), s2 = std::clamp(p[TtSmooth2], 0.0, 1.0);
    const double s3 = std::clamp(p[TtSmooth3], 0.0, 1.0), s4 = std::clamp(p[TtSmooth4], 0.0, 1.0);
    const double a2 = 0.99 + 0.0099 * s2, a3 = 0.99 + 0.00999 * s3;
    const double k = (1.0 - a2) / (std::floor(s1 * 2500.0) + 250.0);
    springA_ = std::pow(a2, r);
    springK_ = k * (1.0 - springA_) / (1.0 - a2);  // the same steady pull at any rate
    springC_ = (1.0 - a3) * (1.0 - s4) * r;
    springCarry_ = s4 * r;
    gainA_ = 1.0 - std::pow(0.99, r);  // output gain / pan smoothing
    dcR_ = std::pow(0.99, r);           // DC blocker pole
    fadeLen_ = std::max(1, static_cast<int>(0.004 * sr_));
    // One revolution = the whole sample; unit = the sample's own speed in revolutions per engine sample.
    unit_ = smp_ ? smp_->sampleRate / sr_ / static_cast<double>(smp_->frames()) : 0.0;
  }

  void render(float* L, float* R, int n) override {
    if (!smp_) {
      publish();
      return;
    }
    const size_t N = smp_->frames();
    const float* sl = smp_->left.data();
    const float* srr = smp_->right.vec().data();
    const bool hand = holdParam_;
    held_ = hand;
    const double motor = keys_ > 0 ? motorTarget(keyPitch_[static_cast<size_t>(keys_ - 1)]) : transport_ * speed_ * unit_;
    const double gTarget = mute_ ? 0.0 : volume_;
    for (int i = 0; i < n; ++i) {
      if (hand) {
        const double err = position_ - pos_;
        vel_ = springK_ * err + springA_ * vel_;
        pos_ += springC_ * err + springCarry_ * vel_;
        wasHand_ = true;
      } else {
        if (wasHand_) {  // let go: the platter continues from where the hand left it
          pos_ -= std::floor(pos_);
          wasHand_ = false;
        }
        vel_ = (vel_ - motor) * motorA_ + motor;
        pos_ += vel_;
        pos_ -= std::floor(pos_);
      }
      double l, r;
      read(sl, srr, N, pos_, l, r);
      if (fadeLeft_ > 0) {
        fadeFrom_ += fadeVel_;
        double fl, fr;
        read(sl, srr, N, fadeFrom_, fl, fr);
        const double t = static_cast<double>(fadeLeft_) / fadeLen_;
        l = l * (1.0 - t) + fl * t;
        r = r * (1.0 - t) + fr * t;
        --fadeLeft_;
      }
      // DC blocker (a slow or stopped platter leaves an offset).
      const double yl = dcR_ * dcy_[0] + l - dcx_[0], yr = dcR_ * dcy_[1] + r - dcx_[1];
      dcx_ = {l, r};
      dcy_ = {yl, yr};
      g_ += (gTarget - g_) * gainA_;
      pg_ += (pan_ - pg_) * gainA_;
      const double gl = pg_ > 0 ? 1.0 - pg_ : 1.0, gr = pg_ < 0 ? 1.0 + pg_ : 1.0;
      L[i] += static_cast<float>(yl * g_ * gl);
      R[i] += static_cast<float>(yr * g_ * gr);
    }
    if (std::fabs(dcy_[0]) < 1e-20) dcy_[0] = 0;
    if (std::fabs(dcy_[1]) < 1e-20) dcy_[1] = 0;
    if (std::fabs(vel_) < 1e-14) vel_ = 0;
    publish();
  }

 private:
  double motorTarget(double pitch) const {
    const double key = keyTrack_ ? std::pow(2.0, (pitch - 60.0) / 12.0) : 1.0;
    return speed_ * key * unit_;
  }
  void removeKey(uint32_t id) {
    for (int i = 0; i < keys_; ++i)
      if (keyIds_[static_cast<size_t>(i)] == id) {
        for (int j = i + 1; j < keys_; ++j) {
          keyIds_[static_cast<size_t>(j - 1)] = keyIds_[static_cast<size_t>(j)];
          keyPitch_[static_cast<size_t>(j - 1)] = keyPitch_[static_cast<size_t>(j)];
        }
        --keys_;
        return;
      }
  }
  // Linear interpolation at revolution `ph` (any value; wraps around the sample).
  static void read(const float* sl, const float* sr, size_t N, double ph, double& l, double& r) {
    double x = (ph - std::floor(ph)) * static_cast<double>(N);
    size_t i0 = static_cast<size_t>(x);
    if (i0 >= N) i0 = 0, x = 0;
    const size_t i1 = i0 + 1 < N ? i0 + 1 : 0;
    const double f = x - static_cast<double>(i0);
    l = sl[i0] + (sl[i1] - sl[i0]) * f;
    r = sr[i0] + (sr[i1] - sr[i0]) * f;
  }
  void publish() {
    tel_.pos.store(static_cast<float>(pos_ - std::floor(pos_)), std::memory_order_relaxed);
    tel_.speed.store(unit_ > 0 ? static_cast<float>(vel_ / unit_) : 0.f, std::memory_order_relaxed);
    tel_.gain.store(static_cast<float>(g_), std::memory_order_relaxed);
    tel_.held.store(held_, std::memory_order_relaxed);
    tel_.loaded.store(smp_ != nullptr, std::memory_order_relaxed);
  }

  const Sample* smp_ = nullptr;
  const Sample* lastSmp_ = nullptr;
  double sr_ = kRefRate;
  // parameters (configure)
  double position_ = 0, speed_ = 1, pan_ = 0, volume_ = 0.78, cue_ = 0, unit_ = 0;
  bool mute_ = false, holdParam_ = false, keyTrack_ = true;
  int transport_ = 0, notesMode_ = TnCue;
  double motorA_ = 0.9999, springA_ = 0.999, springK_ = 0, springC_ = 0, springCarry_ = 0, gainA_ = 0.01, dcR_ = 0.99;
  // platter
  double pos_ = 0, vel_ = 0;
  bool held_ = false, wasHand_ = false;
  // output
  double g_ = 0, pg_ = 0;
  std::array<double, 2> dcx_{0, 0}, dcy_{0, 0};
  // cue jump crossfade
  double fadeFrom_ = 0, fadeVel_ = 0;
  int fadeLeft_ = 0, fadeLen_ = 176;
  // held keys
  std::array<uint32_t, 16> keyIds_{};
  std::array<double, 16> keyPitch_{};
  int keys_ = 0;
  TurntableTelemetry tel_;
};

}  // namespace

const TurntableTelemetry* turntableTelemetry(const Generator* g) {
  const auto* t = dynamic_cast<const TurntableGen*>(g);
  return t ? &t->tel() : nullptr;
}

std::unique_ptr<Generator> createTurntable() { return std::make_unique<TurntableGen>(); }

bool prepareTurntableAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.turntable") return false;
  aux = ChannelAux{};
  // No sample yet: the built-in phrase, so a fresh instance plays.
  aux.samples.push_back(state.data.empty() ? samplers::scratchDefaultSample() : sampleCache().get(state.data));
  return true;
}

void turntableFromReference(const float* v, int n, std::vector<float>& params) {
  params.assign(TtCount, 0.f);
  const PluginInfo* info = findPlugin("gl.turntable");
  if (info)
    for (int i = 0; i < TtCount; ++i) params[static_cast<size_t>(i)] = static_cast<float>(info->params[static_cast<size_t>(i)].def);
  auto at = [&](int i, float d) { return i < n && std::isfinite(v[i]) ? v[i] : d; };
  params[TtPosition] = std::clamp(at(0, 0.f), 0.f, 1.f);
  params[TtMute] = at(1, 1.f) >= 0.5f ? 0.f : 1.f;  // stored: 1 = sound on
  params[TtSpeed] = std::clamp(at(2, 0.5f), 0.f, 1.f) * 2.f;
  params[TtAccel] = std::clamp(at(3, 0.25f), 0.f, 1.f);
  params[TtPan] = std::clamp(at(4, 0.5f), 0.f, 1.f) * 2.f - 1.f;
  params[TtVolume] = std::clamp(at(5, 0.78f), 0.f, 1.f);
  params[TtHold] = at(6, 0.f) >= 0.5f ? 1.f : 0.f;
  params[TtSense] = std::clamp(at(7, 0.5f), 0.f, 1.f);
  params[TtTransport] = static_cast<float>(std::clamp(static_cast<int>(std::lround(at(8, 0.f))), -1, 1) + 1);
  for (int k = 0; k < 4; ++k) params[static_cast<size_t>(TtSmooth1 + k)] = std::clamp(at(9 + k, 0.5f), 0.f, 1.f);
  params[TtTurns] = static_cast<float>(std::clamp(static_cast<int>(std::lround(at(13, 9.f))), 0, 18) - 9);
  params[TtNotes] = static_cast<float>(TnOff);  // the reference plays by its motor, not by notes
}

}  // namespace gl::packs::spectral

// Gridline — Spline Scratcher (gl.splinescratch) runtime: every key plays its
// own drawn patch over the channel sample. Across the patch length (beats at
// the song tempo, scaled by the speed), the position spline gives the read
// point inside the patch's sample window and the volume spline / mute toggles
// gate the output; moving the read point back and forth is the scratch.
#include <algorithm>
#include <cmath>
#include "../../engine/Dsp.h"
#include "metal.h"
#include "metal_rt.h"

namespace gl::packs::models::metal {

namespace {

constexpr int kVoices = 8;

class SplineScratchGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, released = false;
    uint32_t id = 0;
    int key = 60;
    double t = 0;        // seconds into the patch
    float env = 0;       // attack / release envelope
    float gate = 1;      // smoothed mute gate
    float vel = 1;
    double lastPos = -1;  // previous read point (frames)
    uint64_t age = 0;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    sample_ = aux.sample(0);
    data_ = dynamic_cast<const SplineScratchAux*>(aux.custom.get());
    speed_ = prmf(s, SsSpeed, 512) / 512.0;  // 0..1024 = 0..2x (the patch's own speed scales it)
    attackSec_ = std::max(0.0005, prmf(s, SsAttack, 25) / 1000.0);    // 0..100 ms
    releaseSec_ = std::max(0.002, prmf(s, SsRelease, 2500) / 1000.0);       // 0..5000 ms
    winA_ = std::clamp(prmf(s, SsStartWave, 0) / 10000.0f, 0.0f, 1.0f);
    winB_ = std::clamp(prmf(s, SsEndWave, 10000) / 10000.0f, 0.0f, 1.0f);
    speedMode_ = prmi(s, SsSplineMode, 0);
    volume_ = prmf(s, SsVolume, 0.8f);
    continuous_ = prmi(s, SsContinuous, 0) != 0;
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    for (auto& v : v_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick)
      for (auto& v : v_)
        if (!pick || v.age < pick->age) pick = &v;
    Voice& v = *pick;
    v = Voice{};
    v.on = true;
    v.id = id;
    v.key = std::clamp(static_cast<int>(std::lround(pitch)), 0, kSsKeys - 1);
    v.vel = vel;
    v.age = ++clock_;
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : v_)
      if (v.on && v.id == id) v.released = true;
  }
  void onAllOff() override {
    for (auto& v : v_) v.released = true;
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
  }

  void render(float* L, float* R, int n) override {
    const Sample* smp = sample_.get();
    if (!smp || smp->frames() < 2) {
      for (auto& v : v_) v.on = false;
      return;
    }
    const SsPatch def = ssDefaultPatch();
    const double frames = static_cast<double>(smp->frames());
    const double srcRate = smp->sampleRate / sr_;
    for (auto& v : v_) {
      if (!v.on) continue;
      const SsPatch& p = data_ && data_->d.keys[static_cast<size_t>(v.key)].used ? data_->d.keys[static_cast<size_t>(v.key)] : def;
      const double len = std::max(0.01, p.beats * 60.0 / tempo_ / std::max(0.05, speed_ * std::max(0.05f, p.speed)));
      const float aK = static_cast<float>(1.0 - std::exp(-1.0 / (attackSec_ * std::max(0.05f, p.attack) * sr_)));
      const float rK = static_cast<float>(1.0 - std::exp(-5.0 / (releaseSec_ * std::max(0.05f, p.release) * sr_)));  // fades within the release time
      const float gK = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * sr_)));
      const double a = winA_ + (winB_ - winA_) * p.start, b = winA_ + (winB_ - winA_) * p.end;
      for (int i = 0; i < n; ++i) {
        double x = v.t / len;
        if (x >= 1.0) {
          if (continuous_ && !v.released) x = std::fmod(x, 1.0);
          else v.released = true, x = 1.0;
        }
        double pos;
        if (speedMode_ == 0) {
          pos = (a + (b - a) * ssEval(p.pos, static_cast<float>(x))) * (frames - 1);
        } else {
          // Speed mode: the spline is the playback rate (0.5 = stopped, 0 / 1 = full reverse / forward ×2).
          const double rate = (ssEval(p.pos, static_cast<float>(x)) - 0.5) * 4.0 * srcRate;
          pos = (v.lastPos < 0 ? a * (frames - 1) : v.lastPos + rate);
          const double lo = std::min(a, b) * (frames - 1), hi = std::max(a, b) * (frames - 1);
          pos = std::clamp(pos, lo, hi);
        }
        v.lastPos = pos;
        const float target = v.released ? 0.0f : 1.0f;
        v.env += (target - v.env) * (v.released ? rK : aK);
        v.gate += ((ssSoundOn(p, static_cast<float>(x)) ? 1.0f : 0.0f) - v.gate) * gK;
        const float vol = ssEval(p.vol, static_cast<float>(std::min(1.0, x))) * v.gate * v.env * v.vel * volume_;
        const size_t i0 = static_cast<size_t>(std::clamp(pos, 0.0, frames - 2));
        const float f = static_cast<float>(std::clamp(pos - static_cast<double>(i0), 0.0, 1.0));
        const float l = smp->left[i0] + (smp->left[i0 + 1] - smp->left[i0]) * f;
        const float r = smp->right[i0] + (smp->right[i0 + 1] - smp->right[i0]) * f;
        L[i] += l * vol;
        R[i] += r * vol;
        v.t += 1.0 / sr_;
        if (v.released && v.env < 1e-4f) {
          v.on = false;
          break;
        }
      }
    }
  }

 private:
  Voice v_[kVoices];
  uint64_t clock_ = 0;
  double sr_ = 44100, tempo_ = 120, speed_ = 1, attackSec_ = 0.025, releaseSec_ = 0.5;
  float winA_ = 0, winB_ = 1, volume_ = 0.8f;
  int speedMode_ = 0;
  bool continuous_ = false;
  SamplePtr sample_;
  const SplineScratchAux* data_ = nullptr;
};

}  // namespace

std::unique_ptr<Generator> makeSplineScratch() { return std::make_unique<SplineScratchGen>(); }

}  // namespace gl::packs::models::metal

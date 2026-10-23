// Gridline — Grain Scanner (gl.grainscan) runtime: a grain player over the
// channel sample. Grains start every "grain spacing" (logarithmic: 200 units
// per octave), the read head moves by "wave spacing" between two grains (equal
// spacings = original speed, negative = backwards), each grain has an attack
// / hold / release shape; effect depth / speed wobble the grain pitch,
// randomness scatters the read point, stereo offsets the right channel. Keys
// transpose (pitch mode) or pick the start point (percent / step / transient
// modes). Laws: see the J-22 metal-group notes in the task report.
#include <algorithm>
#include <cmath>
#include "../../engine/Dsp.h"
#include "metal.h"
#include "metal_rt.h"

namespace gl::packs::models::metal {

namespace {

constexpr int kVoices = 8;
constexpr int kGrains = 24;  // per voice


class GrainScanGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Grain {
    bool on = false;
    double pos = 0, rate = 1;   // read point (frames), frames per output sample
    int64_t age = 0, len = 1, att = 1, hold = 0;
    float panL = 1, panR = 1, rOff = 0;
  };
  struct Voice {
    bool on = false, released = false;
    uint32_t id = 0;
    double note = 60, head = 0;  // read head (frames)
    double untilNext = 0;        // samples to the next grain
    float vel = 1, env = 0;
    uint64_t age = 0;
    uint32_t rng = 1;
    double wob = 0;  // effect phase
    uint32_t grainCount = 0;
    int stage = 0;   // envelope: 0 attack/hold, 1 decay/sustain, 2 release
    double envT = 0;
    Grain g[kGrains];
  };

  // The n-th transient of the sample (wrapping), the start when none were found.
  double transientAt(const Sample& smp, int n) const {
    (void)smp;
    if (!slices_ || slices_->empty()) return 0.0;
    return static_cast<double>((*slices_)[static_cast<size_t>(n) % slices_->size()]);
  }

  static float rnd(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 16777216.0f;
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    sample_ = aux.sample(0);
    slices_ = &aux.slices;
    for (int i = 0; i < GsCount; ++i) p_[i] = prmf(s, i, 0);
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
    v.note = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.rng = 0x9E3779B9u ^ (static_cast<uint32_t>(clock_) * 2654435761u);
    const Sample* smp = sample_.get();
    const double frames = smp ? static_cast<double>(smp->frames()) : 0.0;
    // Start point; in the key modes other than pitch the key above the root
    // moves it (percent: 1/24 of the sample per key, step: one 16th note per
    // key at the song tempo, transient: the n-th detected transient) and the
    // key no longer transposes.
    double head = std::clamp(p_[GsStart] / 4096.0, 0.0, 1.0) * frames;
    const int trig = static_cast<int>(std::lround(p_[GsTrigger]));
    const int keyOff = std::max(0, static_cast<int>(std::lround(pitch - p_[GsRoot])));
    if (trig == 1) head += frames * keyOff / 24.0;
    else if (trig == 2 && smp) head += keyOff * 0.25 * 60.0 / tempo_ * smp->sampleRate;
    else if (trig == 3 && smp) head = transientAt(*smp, keyOff);
    v.head = frames > 0 ? std::fmod(head, frames + 1) : 0.0;
    if (p_[GsWaveSpace] < 0 && v.head <= 0) v.head = std::max(0.0, frames - 1);  // backwards from the end
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
    if (!smp || smp->frames() < 4) {
      for (auto& v : v_) v.on = false;
      return;
    }
    const double frames = static_cast<double>(smp->frames());
    const double srcRate = smp->sampleRate / sr_;
    // Grain timing (reference behaviour): grain length = attack + hold; the
    // grain spacing and the read-head step are both proportional to the grain
    // length (200 units per octave), so their ratio is the playback speed;
    // tempo lock stretches the whole sample over one bar.
    // Reference laws (J-33): attack / hold ms = round(10·(e^(0.00786365·x) − 1));
    // spacing = (A + H)·s(gs)/256 (≥ 5 samples) and head step = (A + H)·s(ws)/256
    // with s(x) = round(76.8·(e^(0.0023979·|x|) − 1)) (s(611) = 256).
    auto ms = [](double x) { return std::round(10.0 * (std::exp(0.00786365 * std::max(0.0, x)) - 1.0)); };
    auto sp = [](double x) { return std::round(76.8 * (std::exp(0.0023979 * std::fabs(x)) - 1.0)); };
    const double attSec = std::max(1.0, ms(p_[GsAttack])) * 1e-3;
    const double holdSec = ms(p_[GsHold]) * 1e-3;
    const double grainSec = attSec + holdSec;
    const double space = std::max(5.0 / sr_, grainSec * sp(p_[GsGrainSpace]) / 256.0);
    const double ws = std::clamp(static_cast<double>(p_[GsWaveSpace]), -1000.0, 1000.0);
    double step = ws == 0 ? 0.0 : (ws < 0 ? -1.0 : 1.0) * grainSec * sp(ws) / 256.0;  // source seconds per grain
    if (p_[GsTempoLock] >= 0.5f && smp->sampleRate > 0)
      step = (ws < 0 ? -1.0 : 1.0) * smp->seconds() * space / (4.0 * 60.0 / tempo_);
    const double fxDepth = std::clamp(p_[GsFxDepth] / 384.0f, 0.0f, 1.0f);
    const double fxInc = 3.14159265358979 / std::max(4.0, 204.0 - std::clamp(static_cast<double>(p_[GsFxSpeed]), 4.0, 200.0));
    const float randomness = std::clamp(p_[GsRandom] / 256.0f, 0.0f, 1.0f);
    // J-92: the volume knob was never read (0.8 = the level this engine had before).
    const float volume = std::clamp(p_[GsVolume], 0.0f, 1.0f) / 0.8f;
    const float stereo = std::clamp(p_[GsStereo] / 64.0f, 0.0f, 1.0f);
    const bool loop = p_[GsLoop] >= 0.5f, freeze = p_[GsHoldOn] >= 0.5f;
    const int trig = static_cast<int>(std::lround(p_[GsTrigger]));
    const bool envOn = p_[GsEnvOn] >= 0.5f;
    const double eA = std::max(0.001, static_cast<double>(p_[GsEnvAttack])), eH = std::max(0.0, static_cast<double>(p_[GsEnvHold]));
    const double eD = std::max(0.001, static_cast<double>(p_[GsEnvDecay])), eR = std::max(0.002, static_cast<double>(p_[GsEnvRelease]));
    const float eS = std::clamp(p_[GsEnvSustain], 0.0f, 1.0f);
    for (auto& v : v_) {
      if (!v.on) continue;
      const double rate = (trig == 0 ? std::pow(2.0, (v.note - p_[GsRoot]) / 12.0) : 1.0) * srcRate;
      for (int i = 0; i < n; ++i) {
        // Voice envelope (channel volume envelope when enabled, else a gate with a short release).
        float target;
        if (!envOn) {
          v.env += ((v.released ? 0.0f : 1.0f) - v.env) * static_cast<float>(1.0 - std::exp(-1.0 / ((v.released ? 0.05 : 0.002) * sr_)));
          target = v.env;
        } else {
          v.envT += 1.0 / sr_;
          if (v.released && v.stage < 2) v.stage = 2, v.envT = 0;
          if (v.stage == 0) {
            v.env = static_cast<float>(std::min(1.0, v.envT / eA));
            if (v.envT >= eA + eH) v.stage = 1, v.envT = 0;
          } else if (v.stage == 1) {
            v.env = eS + (1.0f - eS) * static_cast<float>(std::exp(-v.envT / (eD / 3.0)));
          } else {
            v.env *= static_cast<float>(std::exp(-1.0 / (eR / 5.0 * sr_)));
          }
          target = v.env;
        }
        if (v.released && target < 1e-4f) {
          v.on = false;
          break;
        }
        // Spawn grains.
        if (v.untilNext <= 0) {
          v.untilNext += space * sr_;
          for (auto& g : v.g)
            if (!g.on) {
              g.on = true;
              g.age = 0;
              g.att = std::max<int64_t>(1, static_cast<int64_t>(attSec * sr_));
              g.hold = static_cast<int64_t>(holdSec * sr_);
              g.len = g.att * 2 + g.hold;
              // Randomness: a grain now and then starts anywhere in the sample.
              g.pos = randomness > 0 && rnd(v.rng) < randomness ? rnd(v.rng) * (frames - 1) : v.head;
              // Effect: the read point swings back by up to the depth × one step.
              g.pos -= fxDepth * std::fabs(step) * smp->sampleRate * std::fabs(std::sin(v.wob));
              v.wob += fxInc;
              g.rate = rate;
              // Stereo: alternate grains lean left / right.
              const float pan = stereo * ((++v.grainCount & 1) ? 1.0f : -1.0f);
              // Constant grain gain 1.42 through a balance pan (reference).
              g.panL = 1.42f * std::min(1.0f, 1.0f - pan), g.panR = 1.42f * std::min(1.0f, 1.0f + pan);
              g.rOff = 0;
              break;
            }
          if (!freeze) v.head += step * smp->sampleRate;
          if (v.head >= frames || v.head < 0) {
            if (loop) v.head = std::fmod(v.head + frames, frames);
            else v.released = true, v.head = std::clamp(v.head, 0.0, frames - 1);
          }
        }
        v.untilNext -= 1;
        // Mix grains.
        float l = 0, r = 0;
        for (auto& g : v.g) {
          if (!g.on) continue;
          float e;
          if (g.age < g.att) e = static_cast<float>(g.age) / static_cast<float>(g.att);
          else if (g.age < g.att + g.hold) e = 1.0f;
          else e = 1.0f - static_cast<float>(g.age - g.att - g.hold) / static_cast<float>(g.att);
          // linear ramps (overlapping windows sum to 1 at the default spacing)
          auto rd = [&](const std::vector<float>& ch, double at) {
            at = std::fmod(at, frames);
            if (at < 0) at += frames;
            // J-36: fmod(-tiny) + frames rounds to frames: index one past the end (ASan, test_determinism).
            if (!(at < frames)) at = 0;
            if (ch.empty()) return 0.0f;
            const size_t i0 = std::min(static_cast<size_t>(at), ch.size() - 1);
            const size_t i1 = i0 + 1 < ch.size() ? i0 + 1 : 0;
            const float f = static_cast<float>(at - static_cast<double>(i0));
            return ch[i0] + (ch[i1] - ch[i0]) * f;
          };
          l += rd(smp->left, g.pos) * e * g.panL;
          r += rd(smp->right, g.pos + g.rOff) * e * g.panR;
          g.pos += g.rate;
          if (++g.age >= g.len) g.on = false;
        }
        const float gain = target * v.vel * volume;
        L[i] += l * gain;
        R[i] += r * gain;
      }
    }
  }

 private:
  Voice v_[kVoices];
  float p_[GsCount] = {};
  uint64_t clock_ = 0;
  double sr_ = 44100, tempo_ = 120;
  SamplePtr sample_;
  const std::vector<int64_t>* slices_ = nullptr;
};

}  // namespace

std::unique_ptr<Generator> makeGrainScan() { return std::make_unique<GrainScanGen>(); }

}  // namespace gl::packs::models::metal

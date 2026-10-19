// Gridline — Spectra Morph (gl.spectramorph) runtime: four corner programs,
// each a set of key zones of resynthesised harmonic frames (amplitude per
// harmonic over time). A note picks the nearest zone of every corner, walks
// the frames over time (each frame lasts `hold` analysis steps; a sustain
// loop repeats while the key is held) and blends the corners bilinearly on X
// / Y (static, or moving on a circle / line / figure eight). The harmonics
// play as a sine bank at the note's pitch, with an amp envelope, optional
// noise layer, drive and chorus / delay / reverb.
#include <algorithm>
#include <cmath>
#include "../../engine/Dsp.h"
#include "metal.h"
#include "metal_rt.h"

namespace gl::packs::models::metal {

namespace {

constexpr int kVoices = 12;
constexpr int kParts = 160;  // harmonics per voice
constexpr int kCtl = 32;

struct ZonePrep {
  int key = 60;
  float gain = 1;
  std::vector<std::vector<float>> amp;  // frame → harmonic amplitudes (padded to the zone's width)
  std::vector<float> level;
  std::vector<double> start;            // frame start (steps)
  double total = 0;                     // steps
  double loopA = -1, loopB = -1;
  int width = 0;
};
struct SmAux final : AuxData {
  std::array<std::vector<ZonePrep>, kSmPrograms> prog;
  float norm = 1;  // loudness normalisation
};

const SmAux& emptyAux() {
  static const SmAux a;
  return a;
}

}  // namespace

// Built-in corner programs for a fresh instance (no imported data): A a bright
// decaying saw-like tone, B a hollow odd-harmonic tone, C a vowel-like formant
// tone, D a bell-like sparse spectrum; each with a short attack motion and a
// sustain loop.
SmData factoryMorph() {
  SmData d;
  for (int p = 0; p < kSmPrograms; ++p) {
    SmZone z;
    z.key = 60;
    for (int f = 0; f < 40; ++f) {
      SmFrame fr;
      fr.hold = 2;
      fr.level = f < 4 ? 0.4f + 0.15f * f : 1.0f - 0.004f * f;
      const float bright = 1.0f - 0.5f * std::min(1.0f, f / 20.0f);
      for (int k = 1; k <= 48; ++k) {
        float a;
        switch (p) {
          case 0: a = 1.0f / k * std::pow(bright, 0.05f * k); break;
          case 1: a = (k % 2) ? 1.0f / k : 0.0f; break;
          case 2: {
            const float f1 = 2.5f + 0.5f * std::sin(f * 0.3f), f2 = 9.0f;
            a = 0.08f + std::exp(-0.5f * (k - f1) * (k - f1)) + 0.6f * std::exp(-0.3f * (k - f2) * (k - f2));
            a /= std::sqrt(static_cast<float>(k));
            break;
          }
          default: a = (k == 1 || k == 3 || k == 7 || k == 12 || k == 19 || k == 28) ? 1.0f / std::sqrt(static_cast<float>(k)) * std::pow(bright, 0.1f * k) : 0.0f; break;
        }
        fr.amp.push_back(a);
      }
      z.frames.push_back(fr);
    }
    z.loopStart = 40, z.loopEnd = 78;
    d.prog[static_cast<size_t>(p)].zones.push_back(z);
  }
  return d;
}

std::shared_ptr<const AuxData> buildSpectraMorphAux(const PluginState& s) {
  SmData d;
  bool any = false;
  if (parseMorph(s.data, d))
    for (const auto& pr : d.prog) any = any || !pr.zones.empty();
  if (!any) d = factoryMorph();
  auto a = std::make_shared<SmAux>();
  float peak = 0;
  for (int p = 0; p < kSmPrograms; ++p)
    for (const SmZone& z : d.prog[static_cast<size_t>(p)].zones) {
      if (z.frames.empty()) continue;
      ZonePrep zp;
      zp.key = z.key, zp.gain = z.gain;
      for (const auto& f : z.frames) zp.width = std::max(zp.width, std::min(kParts, static_cast<int>(f.amp.size())));
      double t = 0;
      for (const auto& f : z.frames) {
        std::vector<float> v(static_cast<size_t>(zp.width), 0.0f);
        float sum = 0;
        for (int k = 0; k < zp.width && k < static_cast<int>(f.amp.size()); ++k) v[static_cast<size_t>(k)] = f.amp[static_cast<size_t>(k)], sum += f.amp[static_cast<size_t>(k)];
        peak = std::max(peak, sum * f.level * z.gain);
        zp.amp.push_back(std::move(v));
        zp.level.push_back(f.level);
        zp.start.push_back(t);
        t += std::max(1, f.hold);
      }
      zp.total = t;
      if (z.loopStart >= 0 && z.loopEnd > z.loopStart && z.loopEnd <= t) zp.loopA = z.loopStart, zp.loopB = z.loopEnd;
      a->prog[static_cast<size_t>(p)].push_back(std::move(zp));
    }
  a->norm = peak > 1e-6f ? 1.0f / peak : 1.0f;
  return a;
}

namespace {

class SpectraMorphGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return fx_.ringing();
  }

 protected:
  struct Voice {
    bool on = false, released = false;
    uint32_t id = 0;
    double note = 60, cur = 60;
    float vel = 1;
    uint64_t age = 0;
    double steps = 0;  // analysis position
    double env = 0;
    int stage = 0;     // 0 attack, 1 decay / sustain, 2 release
    int zone[kSmPrograms] = {-1, -1, -1, -1};
    uint32_t ph[kParts] = {};
    float amp[kParts] = {}, step[kParts] = {};
    int ctl = 0;
    float noiseLp = 0;
    uint32_t rng = 1;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    prep_ = dynamic_cast<const SmAux*>(aux.custom.get());
    if (!prep_) prep_ = &emptyAux();
    vol_ = prmf(s, SmVolume, 0.8f);
    trans_ = prmf(s, SmTranspose, 0) + prmf(s, SmFine, 0) / 100.0f;
    glide_ = prmf(s, SmGlide, 0);
    velSens_ = prmf(s, SmVelSens, 0.7f);
    mx_ = prmf(s, SmMorphX, 0), my_ = prmf(s, SmMorphY, 0);
    rate_ = prmf(s, SmMorphRate, 0);
    mode_ = prmi(s, SmMorphMode, 0);
    speed_ = prmf(s, SmSpeed, 1);
    loop_ = prmi(s, SmLoop, 1);
    att_ = prmf(s, SmAttack, 0.002f), dec_ = prmf(s, SmDecay, 0.5f), sus_ = prmf(s, SmSustain, 1), rel_ = prmf(s, SmRelease, 0.3f);
    noise_ = prmf(s, SmNoise, 0), noiseColor_ = prmf(s, SmNoiseColor, 0.5f);
    drive_ = prmf(s, SmDrive, 0);
    voices_ = std::clamp(prmi(s, SmVoices, 12), 1, kVoices);
    fx_.setDirect(sr_, prmi(s, SmChorusOn, 0) ? prmf(s, SmChorusDepth, 0.5f) : 0.0f, prmi(s, SmDelayOn, 0) ? 0.4f : 0.0f,
                  prmf(s, SmDelayTime, 0.3f), prmf(s, SmDelayFb, 0.4f), prmi(s, SmReverbOn, 0) ? prmf(s, SmReverbMix, 0.3f) : 0.0f,
                  prmf(s, SmReverbDecay, 0.5f));
  }

  int nearestZone(int p, double note) const {
    const auto& zs = prep_->prog[static_cast<size_t>(p)];
    int best = -1;
    double bd = 1e9;
    for (size_t i = 0; i < zs.size(); ++i) {
      const double d = std::fabs(zs[i].key - note);
      if (d < bd) bd = d, best = static_cast<int>(i);
    }
    return best;
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    for (int i = 0; i < voices_; ++i)
      if (!v_[i].on) {
        pick = &v_[i];
        break;
      }
    if (!pick)
      for (int i = 0; i < voices_; ++i)
        if (!pick || v_[i].age < pick->age) pick = &v_[i];
    Voice& v = *pick;
    const double from = lastNote_ >= 0 ? lastNote_ : pitch;
    v = Voice{};
    v.on = true;
    v.id = id;
    v.note = pitch;
    v.cur = glide_ > 0 ? from : pitch;
    v.vel = 1.0f - velSens_ + velSens_ * vel;
    v.age = ++clock_;
    v.rng = 0x1234567u + static_cast<uint32_t>(clock_) * 2654435761u;
    for (int p = 0; p < kSmPrograms; ++p) v.zone[p] = nearestZone(p, pitch);
    lastNote_ = pitch;
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : v_)
      if (v.on && v.id == id && !v.released) v.released = true, v.stage = 2;
  }
  void onAllOff() override {
    for (auto& v : v_)
      if (v.on) v.released = true, v.stage = 2;
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
    fx_.reset();
  }

  // Harmonic amplitudes of one zone at an analysis position (linear between frames).
  static void zoneAt(const ZonePrep& z, double pos, float w, float* out, int n) {
    if (z.amp.empty() || w <= 0) return;
    size_t i = static_cast<size_t>(std::upper_bound(z.start.begin(), z.start.end(), pos) - z.start.begin());
    i = i == 0 ? 0 : i - 1;
    const size_t j = std::min(i + 1, z.amp.size() - 1);
    const double span = j > i ? z.start[j] - z.start[i] : 1.0;
    const float f = j > i ? static_cast<float>(std::clamp((pos - z.start[i]) / span, 0.0, 1.0)) : 0.0f;
    const float li = z.level[i] * z.gain * w, lj = z.level[j] * z.gain * w;
    const int m = std::min(n, z.width);
    for (int k = 0; k < m; ++k)
      out[k] += z.amp[i][static_cast<size_t>(k)] * li * (1 - f) + z.amp[j][static_cast<size_t>(k)] * lj * f;
  }

  void control(Voice& v, double dt) {
    // Glide.
    if (glide_ > 0 && v.cur != v.note) {
      const double k = 1.0 - std::exp(-dt / std::max(0.001f, glide_ / 3));
      v.cur += (v.note - v.cur) * k;
      if (std::fabs(v.note - v.cur) < 0.001) v.cur = v.note;
    }
    // Analysis position (sustain loop while held).
    v.steps += dt / kSmStepSec * speed_;
    // Morph position.
    float x = mx_, y = my_;
    if (mode_ > 0 && rate_ > 0) {
      const double ph = dsp::kTwoPi * rate_ * t_;
      if (mode_ == 1) x = 0.5f + 0.5f * static_cast<float>(std::cos(ph)), y = 0.5f + 0.5f * static_cast<float>(std::sin(ph));
      else if (mode_ == 2) x = 0.5f - 0.5f * static_cast<float>(std::cos(ph));
      else if (mode_ == 3) y = 0.5f - 0.5f * static_cast<float>(std::cos(ph));
      else x = 0.5f + 0.5f * static_cast<float>(std::sin(ph)), y = 0.5f + 0.5f * static_cast<float>(std::sin(2 * ph));
    }
    x = std::clamp(x, 0.0f, 1.0f), y = std::clamp(y, 0.0f, 1.0f);
    float w[kSmPrograms] = {(1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y};
    float wsum = 0;
    for (int p = 0; p < kSmPrograms; ++p) {
      if (v.zone[p] < 0) w[p] = 0;
      wsum += w[p];
    }
    if (wsum <= 1e-6f) {  // only empty corners selected: use the filled ones evenly
      for (int p = 0; p < kSmPrograms; ++p) w[p] = v.zone[p] >= 0 ? 1.0f : 0.0f, wsum += w[p];
    }
    float tgt[kParts] = {};
    if (wsum > 0)
      for (int p = 0; p < kSmPrograms; ++p) {
        if (v.zone[p] < 0 || w[p] <= 0) continue;
        const ZonePrep& z = prep_->prog[static_cast<size_t>(p)][static_cast<size_t>(v.zone[p])];
        double pos = v.steps;
        if (!v.released && z.loopA >= 0 && pos > z.loopB && (loop_ == 1 || loop_ == 2)) {
          const double len = z.loopB - z.loopA;
          if (loop_ == 2) {
            const double u = std::fmod(pos - z.loopA, 2 * len);
            pos = z.loopA + (u < len ? u : 2 * len - u);
          } else {
            pos = z.loopA + std::fmod(pos - z.loopA, len);
          }
        } else if (loop_ == 0 && pos >= z.total && !v.released) {
          v.released = true, v.stage = 2;  // one-shot: the sound ends with its last frame
        }
        pos = std::min(pos, std::max(0.0, z.total - 1));
        zoneAt(z, pos, w[p] / wsum, tgt, kParts);
      }
    // Envelope.
    if (v.stage == 0) {
      v.env += dt / std::max(0.0005f, att_);
      if (v.env >= 1) v.env = 1, v.stage = 1;
    } else if (v.stage == 1) {
      v.env = sus_ + (v.env - sus_) * std::exp(-dt / std::max(0.001f, dec_ / 3));
    } else {
      v.env *= std::exp(-dt / std::max(0.001f, rel_ / 5));
      if (v.env < 1e-4) v.on = false;
    }
    const double f0 = dsp::noteToHz(v.cur + trans_);
    const float g = static_cast<float>(v.env) * v.vel * vol_ * prep_->norm * 0.6f;
    for (int k = 0; k < kParts; ++k) {
      const double hz = f0 * (k + 1);
      const float a = hz < sr_ * 0.45 ? tgt[k] * g : 0.0f;
      v.step[k] = (a - v.amp[k]) / kCtl;
    }
    inc0_ = f0;
  }

  void render(float* L, float* R, int n) override {
    const SineTable& sine = sineTable();
    const double dt = kCtl / sr_;
    for (int i = 0; i < n; ++i) {
      float out = 0;
      for (auto& v : v_) {
        if (!v.on) continue;
        if (v.ctl <= 0) {
          control(v, dt);
          v.ctl = kCtl;
          const double f0 = inc0_;
          for (int k = 0; k < kParts; ++k) incs_[k] = phaseInc(f0 * (k + 1), sr_);
          for (int k = 0; k < kParts; ++k) v.step[k] = incs_[k] ? v.step[k] : -v.amp[k] / kCtl;
          std::copy(incs_, incs_ + kParts, vinc_[&v - v_]);
          if (!v.on) continue;
        }
        --v.ctl;
        float s = 0;
        const uint32_t* inc = vinc_[&v - v_];
        for (int k = 0; k < kParts; ++k) {
          if (v.amp[k] == 0 && v.step[k] == 0) continue;
          v.ph[k] += inc[k];
          s += sine.at(v.ph[k]) * v.amp[k];
          v.amp[k] = std::max(0.0f, v.amp[k] + v.step[k]);
        }
        if (noise_ > 0) {
          v.rng = v.rng * 1664525u + 1013904223u;
          const float wn = static_cast<float>(v.rng >> 8) / 8388608.0f - 1.0f;
          v.noiseLp += (wn - v.noiseLp) * (0.05f + 0.9f * noiseColor_);
          s += v.noiseLp * noise_ * static_cast<float>(v.env) * v.vel * vol_ * 0.2f;
        }
        out += s;
      }
      if (drive_ > 0) out = std::tanh(out * (1 + 6 * drive_)) / (1 + 2 * drive_);
      L[i] += out;
      R[i] += out;
    }
    t_ += n / sr_;
    fx_.process(L, R, n);
  }

 private:
  Voice v_[kVoices];
  uint32_t incs_[kParts] = {};
  uint32_t vinc_[kVoices][kParts] = {};
  const SmAux* prep_ = nullptr;
  ModelFx fx_;
  double sr_ = 44100, t_ = 0, inc0_ = 0, lastNote_ = -1;
  float vol_ = 0.8f, trans_ = 0, glide_ = 0, velSens_ = 0.7f, mx_ = 0, my_ = 0, rate_ = 0, speed_ = 1;
  float att_ = 0.002f, dec_ = 0.5f, sus_ = 1, rel_ = 0.3f, noise_ = 0, noiseColor_ = 0.5f, drive_ = 0;
  int mode_ = 0, loop_ = 1, voices_ = 12;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeSpectraMorph() { return std::make_unique<SpectraMorphGen>(); }

}  // namespace gl::packs::models::metal

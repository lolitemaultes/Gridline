// Gridline — Step Bass (gl.stepbass): a monophonic transistor-style bass
// line voice. Saw / pulse blend (pulse width + PWM LFO), a resonant 24 dB
// ladder low pass with key follow, swept by a decaying envelope (env mod;
// accented notes use the shorter "min decay" and open further), a short
// gate VCA, attack clicks, output high pass and volume; then distortion
// (pre high pass, drive, tone, level), a tempo-synced stereo delay (time /
// right tap / feedback / tone / stereo) and a small reverb. Overlapping notes
// slide (legato). The 32-step sequencer plays the selected program's
// sequence while a key is held (transposed by key − sequence key), at the
// song tempo or the patch's own tempo; steps carry gate / accent / slide.
// The bank (128 programs: patches + sequences) lives in PluginState::data
// as text, parsed on the UI thread into the aux data.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include "models_rt.h"
#include "strings.h"
#include "strings_laws.h"

namespace gl::packs::models {

using namespace strs;

namespace {

using namespace dsp;

struct SbBank : AuxData {
  struct Seq {
    int length = 16;
    double bpm = 125;
    int note[kSbSteps];
    bool gate[kSbSteps], accent[kSbSteps], slide[kSbSteps];
    Seq() {
      for (int i = 0; i < kSbSteps; ++i) note[i] = 60, gate[i] = true, accent[i] = slide[i] = false;
    }
  };
  Seq seqs[kSbPrograms];
  int progSeq[kSbPrograms];
  SbBank() {
    for (int i = 0; i < kSbPrograms; ++i) progSeq[i] = i;
  }
};

const SbBank kDefaultBank;  // all programs: 16 steps of C, gate on

class StepBassGen final : public Generator {
 public:
  StepBassGen() {
    dly_[0].assign(static_cast<size_t>(192000 * 2.1) + 8, 0.0f);
    dly_[1] = dly_[0];
    for (int i = 0; i < 4; ++i) rev_[i].assign(static_cast<size_t>(192000 * 0.2) + 8, 0.0f);
    pre_.assign(static_cast<size_t>(192000 * 0.25) + 8, 0.0f);
  }
  bool active() const override { return gate_ || amp_ > 1e-5 || held_ > 0 || tail_ > 0; }
  bool supportsGlide() const override { return true; }

 private:
  float p(int i) const { return p_[static_cast<size_t>(i)]; }

  void configure(const PluginState& st, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    songBpm_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    for (int i = 0; i < SbCount; ++i) p_[static_cast<size_t>(i)] = prmf(st, i, p_[static_cast<size_t>(i)]);
    bank_ = dynamic_cast<const SbBank*>(aux.custom.get());
    seqOn_ = p(SbSeqOn) >= 0.5f;
    prog_ = std::clamp(static_cast<int>(std::lround(p(SbProgram))), 0, kSbPrograms - 1);
    root_ = std::clamp(static_cast<int>(std::lround(p(SbRoot))), 0, 127);
    const double bpm = p(SbTempoMode) >= 0.5f ? std::clamp(static_cast<double>(p(SbBpm)), 10.0, 522.0) : songBpm_;
    stepLen_ = sr_ * 60.0 / bpm / 4.0;
    hpCoef_ = std::exp(-kTwoPi * law::sbHighPassHz(p(SbHighPass)) / sr_);
    vol_ = 2.0 * p(SbVolume) * p(SbVolume);
    // Effects.
    distOn_ = p(SbDistOn) >= 0.5f;
    distHp_ = std::exp(-kTwoPi * law::expMap(p(SbDistHp), 10.0, 2000.0) / sr_);
    distDrive_ = law::expMap(p(SbDistDrive), 1.0, 60.0);
    distTone_ = std::exp(-kTwoPi * law::expMap(p(SbDistTone), 400.0, 20000.0) / sr_);
    distVol_ = 2.0 * p(SbDistVolume) * p(SbDistVolume);
    dlyOn_ = p(SbDlyOn) >= 0.5f;
    if (p(SbDlySync) >= 0.5f) {
      static const double whole[kSbDelayDivs] = {1.0 / 64, 1.0 / 32, 1.0 / 24, 3.0 / 64, 1.0 / 16, 1.0 / 12, 3.0 / 32, 3.0 / 16};
      const int k = std::clamp(static_cast<int>(std::lround(p(SbDlyTime) * (kSbDelayDivs - 1))), 0, kSbDelayDivs - 1);
      dlySec_ = whole[k] * 4.0 * 60.0 / bpm;
    } else {
      dlySec_ = law::expMap(p(SbDlyTime), 0.01, 1.0);
    }
    dlySec_ = std::clamp(dlySec_, 0.001, 2.0);
    dlyRight_ = std::clamp(dlySec_ * (0.25 + 0.75 * p(SbDlyRightTap)) * 2.0, 0.001, 2.0);
    dlyAmt_ = p(SbDlyAmount);
    dlyFb_ = 0.95 * p(SbDlyFeedback);
    dlyTone_ = std::exp(-kTwoPi * law::expMap(p(SbDlyTone), 300.0, 18000.0) / sr_);
    dlyStereo_ = p(SbDlyStereo);
    revOn_ = p(SbRevOn) >= 0.5f;
    const double t60 = law::expMap(p(SbRevDecay), 0.2, 10.0);
    const double size = 0.4 + 1.2 * p(SbRevSize);
    static const double kMs[4] = {31.3, 37.9, 43.1, 47.7};
    for (int i = 0; i < 4; ++i) {
      revLen_[i] = std::clamp<size_t>(static_cast<size_t>(kMs[i] * 0.001 * sr_ * size), 1, rev_[i].size() - 1);
      revG_[i] = std::pow(10.0, -3.0 * (static_cast<double>(revLen_[i]) / sr_) / t60);
    }
    revDamp_ = std::exp(-kTwoPi * law::expMap(1.0 - p(SbRevHiDamp), 1500.0, 20000.0) / sr_);
    revHi_ = std::exp(-kTwoPi * law::expMap(p(SbRevHiCut), 1000.0, 20000.0) / sr_);
    revLo_ = std::exp(-kTwoPi * law::expMap(p(SbRevLowCut), 20.0, 1000.0) / sr_);
    preLen_ = std::clamp<size_t>(static_cast<size_t>(0.2 * p(SbRevPredelay) * sr_), 1, pre_.size() - 1);
    preFb_ = 0.8 * p(SbRevPreFb);
    revWet_ = 1.5 * p(SbRevWet);
    revDry_ = 1.2 * p(SbRevDry);
    revEr_ = p(SbRevEr);
    revWidth_ = p(SbRevWidth);
    revDiff_ = 0.2 + 0.5 * p(SbRevDiffusion);
  }

  // ---- notes
  void startNote(int note, bool accent, bool slide) {
    target_ = note + 24.0 * (p(SbTune) - 0.5);
    if (slide && gate_) {
      glideLeft_ = static_cast<int>(0.06 * sr_);  // reference-style fixed slide (~60 ms)
      glideStep_ = (target_ - pitch_) / glideLeft_;
    } else {
      pitch_ = target_;
      glideLeft_ = 0;
      env_ = 1.0;
      accEnv_ = accent ? 1.0 : 0.0;
      click_ = p(SbClicks);
    }
    accent_ = accent;
    gate_ = true;
  }
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    const int key = static_cast<int>(std::lround(pitch));
    if (held_ < 16) heldIds_[held_] = id, heldKeys_[held_] = key, ++held_;
    if (seqOn_) {
      if (held_ == 1) {
        step_ = -1;
        stepPos_ = stepLen_;  // fire step 0 on the next sample
      }
      seqKey_ = key;
      return;
    }
    startNote(key, velocity > 0.8f, held_ > 1);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < held_; ++i)
      if (heldIds_[i] == id) {
        for (int j = i; j + 1 < held_; ++j) heldIds_[j] = heldIds_[j + 1], heldKeys_[j] = heldKeys_[j + 1];
        --held_;
        break;
      }
    if (seqOn_) {
      if (held_ > 0) seqKey_ = heldKeys_[held_ - 1];
      else gate_ = false;
      return;
    }
    if (held_ > 0)
      startNote(heldKeys_[held_ - 1], accent_, true);  // back to the previous key (legato)
    else
      gate_ = false;
  }
  void onGlide(uint32_t, double pitch, double seconds) override {
    target_ = pitch + 24.0 * (p(SbTune) - 0.5);
    glideLeft_ = std::max(1, static_cast<int>(seconds * sr_));
    glideStep_ = (target_ - pitch_) / glideLeft_;
  }
  void onAllOff() override {
    held_ = 0;
    gate_ = false;
  }
  void onKill() override {
    held_ = 0;
    gate_ = false;
    amp_ = 0;
    tail_ = 0;
    for (auto& d : dly_) std::fill(d.begin(), d.end(), 0.0f);
    for (auto& d : rev_) std::fill(d.begin(), d.end(), 0.0f);
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    for (double& s : lad_) s = 0;
  }

  void sequencerTick() {
    if (!seqOn_ || held_ == 0) return;
    stepPos_ += 1.0;
    if (stepPos_ < stepLen_) {
      if (gate_ && !curSlide_ && stepPos_ >= 0.5 * stepLen_) gate_ = false;  // gate length: half a step unless sliding
      return;
    }
    stepPos_ -= stepLen_;
    const SbBank& b = bank_ ? *bank_ : kDefaultBank;
    const SbBank::Seq& s = b.seqs[std::clamp(b.progSeq[prog_], 0, kSbPrograms - 1)];
    const int len = std::clamp(s.length, 1, kSbSteps);
    const int prevStep = step_;
    step_ = (step_ + 1) % len;
    const bool prevSlide = prevStep >= 0 && s.slide[prevStep] && s.gate[prevStep];
    if (!s.gate[step_]) {
      gate_ = false;
      curSlide_ = false;
      return;
    }
    startNote(s.note[step_] + (seqKey_ - root_), s.accent[step_], prevSlide);
    curSlide_ = s.slide[step_];
  }

  void render(float* L, float* R, int n) override {
    const double wave = p(SbWave), reso = p(SbReso), envMod = p(SbEnvMod), accAmt = p(SbAccent), keyF = p(SbKeyFollow);
    const double cutBase = law::sbCutoffHz(p(SbCutoff));
    const double decN = std::exp(-1.0 / (law::sbDecaySec(p(SbDecay)) * 0.25 * sr_));
    const double decA = std::exp(-1.0 / (law::sbDecaySec(std::min(p(SbDecay), p(SbMinDecay))) * 0.25 * sr_));
    const double pw0 = std::clamp(0.5 + 0.45 * (p(SbPulseWidth) - 0.1) / 0.9, 0.05, 0.95);
    const double lfoInc = law::sbLfoHz(p(SbLfoRate)) / sr_, lfoAmt = 0.4 * p(SbLfoPw);
    const bool classic = p(SbClassicPulse) >= 0.5f;
    const double ampAtk = 1.0 - std::exp(-1.0 / (0.003 * sr_)), ampRel = 1.0 - std::exp(-1.0 / (0.008 * sr_));
    bool loud = false;
    for (int i = 0; i < n; ++i) {
      sequencerTick();
      if (glideLeft_ > 0) {
        pitch_ += glideStep_;
        if (--glideLeft_ == 0) pitch_ = target_;
      }
      // Oscillator.
      const double hz = noteToHz(pitch_), dt = std::min(0.45, hz / sr_);
      lfoPh_ += lfoInc;
      if (lfoPh_ >= 1) lfoPh_ -= 1;
      const double pw = std::clamp(pw0 + lfoAmt * std::sin(kTwoPi * lfoPh_), 0.05, 0.95);
      double saw = 2.0 * ph_ - 1.0 - polyBlep(ph_, dt);
      double pul = (ph_ < pw ? 1.0 : -1.0) + polyBlep(ph_, dt) - polyBlep(std::fmod(ph_ + 1.0 - pw, 1.0), dt);
      if (classic) pul = std::tanh(3.0 * (saw + 2.0 * pw - 1.0)) * 0.9;  // pulse shaped from the ramp (transistor-style)
      ph_ += dt;
      if (ph_ >= 1) ph_ -= 1;
      double x = (1.0 - wave) * saw + wave * pul;
      // Envelopes.
      env_ *= accent_ ? decA : decN;
      accEnv_ *= decA;
      const double ampT = gate_ ? 1.0 + (accent_ ? accAmt : 0.0) : 0.0;
      amp_ += (ampT - amp_) * (ampT > amp_ ? ampAtk : ampRel);
      // Ladder low pass.
      double fc = cutBase * std::pow(2.0, keyF * 2.0 * (pitch_ - 60.0) / 24.0 + 4.0 * envMod * env_ + 2.5 * accAmt * accEnv_);
      fc = std::clamp(fc, 20.0, sr_ * 0.45);
      const double g = 1.0 - std::exp(-kTwoPi * fc / sr_);
      const double k = 4.1 * reso;
      double in = std::tanh(x - k * lad_[3]);
      for (int s = 0; s < 4; ++s) {
        lad_[s] += g * (in - lad_[s]);
        in = lad_[s];
      }
      double y = lad_[3] * (1.0 + 0.5 * k) * amp_;
      if (click_ > 0) {
        y += click_ * 0.8;
        click_ = click_ > 1e-4 ? click_ * 0.7 : 0.0;
      }
      // Output high pass, volume.
      hpZ_ = y + (hpZ_ - y) * hpCoef_;
      y = (y - hpZ_) * vol_;
      if (distOn_) {
        dHp_ = y + (dHp_ - y) * distHp_;
        double d = std::tanh((y - dHp_) * distDrive_);
        dLp_ = d + (dLp_ - d) * distTone_;
        y = dLp_ * distVol_;
      }
      double l = y, r = y;
      if (dlyOn_) {
        const size_t N = dly_[0].size();
        const size_t rl = (dlyW_ + N - std::max<size_t>(1, static_cast<size_t>(dlySec_ * sr_))) % N;
        const size_t rr = (dlyW_ + N - std::max<size_t>(1, static_cast<size_t>(dlyRight_ * sr_))) % N;
        double dl = dly_[0][rl], dr = dly_[1][rr];
        dTone_[0] = dl + (dTone_[0] - dl) * dlyTone_;
        dTone_[1] = dr + (dTone_[1] - dr) * dlyTone_;
        const double cross = 0.5 * dlyStereo_;
        dly_[0][dlyW_] = static_cast<float>(y + dlyFb_ * ((1 - cross) * dTone_[0] + cross * dTone_[1]));
        dly_[1][dlyW_] = static_cast<float>(y + dlyFb_ * ((1 - cross) * dTone_[1] + cross * dTone_[0]));
        dlyW_ = (dlyW_ + 1) % N;
        l += dlyAmt_ * dTone_[0];
        r += dlyAmt_ * dTone_[1];
      }
      if (revOn_) {
        const size_t PN = pre_.size();
        double pd = pre_[(preW_ + PN - preLen_) % PN];
        pre_[preW_] = static_cast<float>(0.5 * (l + r) + preFb_ * pd);
        preW_ = (preW_ + 1) % PN;
        double o[4], s = 0;
        for (int k2 = 0; k2 < 4; ++k2) {
          const size_t N = rev_[k2].size();
          o[k2] = rev_[k2][(revW_[k2] + N - revLen_[k2]) % N];
          s += o[k2];
        }
        s *= 0.5;
        for (int k2 = 0; k2 < 4; ++k2) {
          const size_t N = rev_[k2].size();
          double v = (o[k2] - s) * revG_[k2];
          revLp_[k2] = v + (revLp_[k2] - v) * revDamp_;
          double w = pd * revDiff_ + revLp_[k2];
          rev_[k2][revW_[k2]] = static_cast<float>(std::fabs(w) < 1e-20 ? 0.0 : w);
          revW_[k2] = (revW_[k2] + 1) % N;
        }
        double wl = o[0] + o[2] + revEr_ * pd, wr = o[1] + o[3] + revEr_ * pd;
        const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * revWidth_;
        wl = mid + side;
        wr = mid - side;
        rHi_[0] = wl + (rHi_[0] - wl) * revHi_;
        rHi_[1] = wr + (rHi_[1] - wr) * revHi_;
        rLo_[0] = rHi_[0] + (rLo_[0] - rHi_[0]) * revLo_;
        rLo_[1] = rHi_[1] + (rLo_[1] - rHi_[1]) * revLo_;
        l = l * revDry_ + revWet_ * 0.3 * (rHi_[0] - rLo_[0]);
        r = r * revDry_ + revWet_ * 0.3 * (rHi_[1] - rLo_[1]);
      }
      if (std::fabs(l) + std::fabs(r) > 1e-6) loud = true;
      L[i] += static_cast<float>(l);
      R[i] += static_cast<float>(r);
    }
    for (double& s : lad_)
      if (std::fabs(s) < 1e-20) s = 0;
    tail_ = loud ? static_cast<int64_t>(sr_ * 6) : tail_ - n;
  }

  std::array<float, SbCount> p_ = [] {
    std::array<float, SbCount> a{};
    a.fill(0.5f);
    return a;
  }();
  const SbBank* bank_ = nullptr;
  double sr_ = 44100, songBpm_ = 120, stepLen_ = 5000, stepPos_ = 0;
  bool seqOn_ = false, gate_ = false, accent_ = false, curSlide_ = false;
  int prog_ = 0, root_ = 60, step_ = -1, seqKey_ = 60, held_ = 0, heldKeys_[16] = {}, glideLeft_ = 0;
  uint32_t heldIds_[16] = {};
  double pitch_ = 60, target_ = 60, glideStep_ = 0, ph_ = 0, lfoPh_ = 0, env_ = 0, accEnv_ = 0, amp_ = 0, click_ = 0;
  double lad_[4] = {0, 0, 0, 0}, hpZ_ = 0, hpCoef_ = 0.99, vol_ = 0.5;
  bool distOn_ = false, dlyOn_ = false, revOn_ = false;
  double distHp_ = 0, distDrive_ = 1, distTone_ = 0, distVol_ = 0.5, dHp_ = 0, dLp_ = 0;
  double dlySec_ = 0.3, dlyRight_ = 0.3, dlyAmt_ = 0, dlyFb_ = 0, dlyTone_ = 0, dlyStereo_ = 0, dTone_[2] = {0, 0};
  std::vector<float> dly_[2], rev_[4], pre_;
  size_t dlyW_ = 0, preW_ = 0, preLen_ = 1, revW_[4] = {0, 0, 0, 0}, revLen_[4] = {1, 1, 1, 1};
  double revG_[4] = {}, revLp_[4] = {}, revDamp_ = 0, revHi_ = 0, revLo_ = 0, preFb_ = 0, revWet_ = 0, revDry_ = 1, revEr_ = 0,
         revWidth_ = 1, revDiff_ = 0.3, rHi_[2] = {0, 0}, rLo_[2] = {0, 0};
  int64_t tail_ = 0;
};

}  // namespace

// Parses the bank text written by the FL import (or the editor): lines
// "prog k patch seq", "seq k length bpm note:gate:accent:slide ×32 | name".
std::shared_ptr<const AuxData> buildStepBassAux(const PluginState& s) {
  auto b = std::make_shared<SbBank>();
  std::istringstream in(s.data);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    int k = -1;
    if (kind == "prog") {
      int patch = 0, seq = 0;
      if (ls >> k >> patch >> seq && k >= 0 && k < kSbPrograms && seq >= 0 && seq < kSbPrograms) b->progSeq[k] = seq;
    } else if (kind == "seq") {
      int len = 16;
      double bpm = 125;
      if (!(ls >> k >> len >> bpm) || k < 0 || k >= kSbPrograms) continue;
      SbBank::Seq& q = b->seqs[k];
      q.length = std::clamp(len, 1, kSbSteps);
      q.bpm = std::isfinite(bpm) ? bpm : 125.0;
      for (int i = 0; i < kSbSteps; ++i) {
        std::string tok;
        if (!(ls >> tok) || tok == "|") break;
        int nt = 60, g = 1, a = 0, sl = 0;
        if (std::sscanf(tok.c_str(), "%d:%d:%d:%d", &nt, &g, &a, &sl) == 4) {
          q.note[i] = std::clamp(nt, 0, 127);
          q.gate[i] = g != 0;
          q.accent[i] = a != 0;
          q.slide[i] = sl != 0;
        }
      }
    }
  }
  return b;
}

std::unique_ptr<Generator> makeStepBass() { return std::make_unique<StepBassGen>(); }

}  // namespace gl::packs::models
